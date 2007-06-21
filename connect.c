#include "git-compat-util.h"
#include "cache.h"
#include "pkt-line.h"
#include "quote.h"
#include "refs.h"
#include "run-command.h"
#include "remote.h"

static char *server_capabilities;

static int check_ref(const char *name, int len, unsigned int flags)
{
	if (!flags)
		return 1;

	if (len < 5 || memcmp(name, "refs/", 5))
		return 0;

	/* Skip the "refs/" part */
	name += 5;
	len -= 5;

	/* REF_NORMAL means that we don't want the magic fake tag refs */
	if ((flags & REF_NORMAL) && check_ref_format(name) < 0)
		return 0;

	/* REF_HEADS means that we want regular branch heads */
	if ((flags & REF_HEADS) && !memcmp(name, "heads/", 6))
		return 1;

	/* REF_TAGS means that we want tags */
	if ((flags & REF_TAGS) && !memcmp(name, "tags/", 5))
		return 1;

	/* All type bits clear means that we are ok with anything */
	return !(flags & ~REF_NORMAL);
}

/*
 * Read all the refs from the other end
 */
struct ref **get_remote_heads(int in, struct ref **list,
			      int nr_match, char **match,
			      unsigned int flags)
{
	*list = NULL;
	for (;;) {
		struct ref *ref;
		unsigned char old_sha1[20];
		static char buffer[1000];
		char *name;
		int len, name_len;

		len = packet_read_line(in, buffer, sizeof(buffer));
		if (!len)
			break;
		if (buffer[len-1] == '\n')
			buffer[--len] = 0;

		if (len < 42 || get_sha1_hex(buffer, old_sha1) || buffer[40] != ' ')
			die("protocol error: expected sha/ref, got '%s'", buffer);
		name = buffer + 41;

		name_len = strlen(name);
		if (len != name_len + 41) {
			if (server_capabilities)
				free(server_capabilities);
			server_capabilities = xstrdup(name + name_len + 1);
		}

		if (!check_ref(name, name_len, flags))
			continue;
		if (nr_match && !path_match(name, nr_match, match))
			continue;
		ref = xcalloc(1, sizeof(*ref) + len - 40);
		hashcpy(ref->old_sha1, old_sha1);
		memcpy(ref->name, buffer + 41, len - 40);
		*list = ref;
		list = &ref->next;
	}
	return list;
}

int server_supports(const char *feature)
{
	return server_capabilities &&
		strstr(server_capabilities, feature) != NULL;
}

int get_ack(int fd, unsigned char *result_sha1)
{
	static char line[1000];
	int len = packet_read_line(fd, line, sizeof(line));

	if (!len)
		die("git-fetch-pack: expected ACK/NAK, got EOF");
	if (line[len-1] == '\n')
		line[--len] = 0;
	if (!strcmp(line, "NAK"))
		return 0;
	if (!prefixcmp(line, "ACK ")) {
		if (!get_sha1_hex(line+4, result_sha1)) {
			if (strstr(line+45, "continue"))
				return 2;
			return 1;
		}
	}
	die("git-fetch_pack: expected ACK/NAK, got '%s'", line);
}

int path_match(const char *path, int nr, char **match)
{
	int i;
	int pathlen = strlen(path);

	for (i = 0; i < nr; i++) {
		char *s = match[i];
		int len = strlen(s);

		if (!len || len > pathlen)
			continue;
		if (memcmp(path + pathlen - len, s, len))
			continue;
		if (pathlen > len && path[pathlen - len - 1] != '/')
			continue;
		*s = 0;
		return (i + 1);
	}
	return 0;
}

enum protocol {
	PROTO_LOCAL = 1,
	PROTO_SSH,
	PROTO_GIT,
};

static enum protocol get_protocol(const char *name)
{
	if (!strcmp(name, "ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "git"))
		return PROTO_GIT;
	if (!strcmp(name, "git+ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "ssh+git"))
		return PROTO_SSH;
	die("I don't handle protocol '%s'", name);
}

#define STR_(s)	# s
#define STR(s)	STR_(s)

#ifndef NO_IPV6

static const char *ai_name(const struct addrinfo *ai)
{
	static char addr[INET_ADDRSTRLEN];
	if ( AF_INET == ai->ai_family ) {
		struct sockaddr_in *in;
		in = (struct sockaddr_in *)ai->ai_addr;
		inet_ntop(ai->ai_family, &in->sin_addr, addr, sizeof(addr));
	} else if ( AF_INET6 == ai->ai_family ) {
		struct sockaddr_in6 *in;
		in = (struct sockaddr_in6 *)ai->ai_addr;
		inet_ntop(ai->ai_family, &in->sin6_addr, addr, sizeof(addr));
	} else {
		strcpy(addr, "(unknown)");
	}
	return addr;
}

/*
 * Returns a connected socket() fd, or else die()s.
 */
static int git_tcp_connect_sock(char *host, int flags)
{
	int sockfd = -1, saved_errno = 0;
	char *colon, *end;
	const char *port = STR(DEFAULT_GIT_PORT);
	struct addrinfo hints, *ai0, *ai;
	int gai;
	int cnt = 0;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
		if (!*port)
			port = "<none>";
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "Looking up %s ... ", host);

	gai = getaddrinfo(host, port, &hints, &ai);
	if (gai)
		die("Unable to look up %s (port %s) (%s)", host, port, gai_strerror(gai));

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\nConnecting to %s (port %s) ... ", host, port);

	for (ai0 = ai; ai; ai = ai->ai_next) {
		sockfd = socket(ai->ai_family,
				ai->ai_socktype, ai->ai_protocol);
		if (sockfd < 0) {
			saved_errno = errno;
			continue;
		}
		if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) < 0) {
			saved_errno = errno;
			fprintf(stderr, "%s[%d: %s]: errno=%s\n",
				host,
				cnt,
				ai_name(ai),
				strerror(saved_errno));
			close(sockfd);
			sockfd = -1;
			continue;
		}
		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "%s ", ai_name(ai));
		break;
	}

	freeaddrinfo(ai0);

	if (sockfd < 0)
		die("unable to connect a socket (%s)", strerror(saved_errno));

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\n");

	return sockfd;
}

#else /* NO_IPV6 */

/*
 * Returns a connected socket() fd, or else die()s.
 */
static int git_tcp_connect_sock(char *host, int flags)
{
	int sockfd = -1, saved_errno = 0;
	char *colon, *end;
	char *port = STR(DEFAULT_GIT_PORT), *ep;
	struct hostent *he;
	struct sockaddr_in sa;
	char **ap;
	unsigned int nport;
	int cnt;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
	}

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "Looking up %s ... ", host);

	he = gethostbyname(host);
	if (!he)
		die("Unable to look up %s (%s)", host, hstrerror(h_errno));
	nport = strtoul(port, &ep, 10);
	if ( ep == port || *ep ) {
		/* Not numeric */
		struct servent *se = getservbyname(port,"tcp");
		if ( !se )
			die("Unknown port %s\n", port);
		nport = se->s_port;
	}

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\nConnecting to %s (port %s) ... ", host, port);

	for (cnt = 0, ap = he->h_addr_list; *ap; ap++, cnt++) {
		sockfd = socket(he->h_addrtype, SOCK_STREAM, 0);
		if (sockfd < 0) {
			saved_errno = errno;
			continue;
		}

		memset(&sa, 0, sizeof sa);
		sa.sin_family = he->h_addrtype;
		sa.sin_port = htons(nport);
		memcpy(&sa.sin_addr, *ap, he->h_length);

		if (connect(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
			saved_errno = errno;
			fprintf(stderr, "%s[%d: %s]: errno=%s\n",
				host,
				cnt,
				inet_ntoa(*(struct in_addr *)&sa.sin_addr),
				strerror(saved_errno));
			close(sockfd);
			sockfd = -1;
			continue;
		}
		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "%s ",
				inet_ntoa(*(struct in_addr *)&sa.sin_addr));
		break;
	}

	if (sockfd < 0)
		die("unable to connect a socket (%s)", strerror(saved_errno));

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\n");

	return sockfd;
}

#endif /* NO_IPV6 */


static void git_tcp_connect(int fd[2], char *host, int flags)
{
	int sockfd = git_tcp_connect_sock(host, flags);

	fd[0] = sockfd;
	fd[1] = dup(sockfd);
}


static char *git_proxy_command;
static const char *rhost_name;
static int rhost_len;

static int git_proxy_command_options(const char *var, const char *value)
{
	if (!strcmp(var, "core.gitproxy")) {
		const char *for_pos;
		int matchlen = -1;
		int hostlen;

		if (git_proxy_command)
			return 0;
		/* [core]
		 * ;# matches www.kernel.org as well
		 * gitproxy = netcatter-1 for kernel.org
		 * gitproxy = netcatter-2 for sample.xz
		 * gitproxy = netcatter-default
		 */
		for_pos = strstr(value, " for ");
		if (!for_pos)
			/* matches everybody */
			matchlen = strlen(value);
		else {
			hostlen = strlen(for_pos + 5);
			if (rhost_len < hostlen)
				matchlen = -1;
			else if (!strncmp(for_pos + 5,
					  rhost_name + rhost_len - hostlen,
					  hostlen) &&
				 ((rhost_len == hostlen) ||
				  rhost_name[rhost_len - hostlen -1] == '.'))
				matchlen = for_pos - value;
			else
				matchlen = -1;
		}
		if (0 <= matchlen) {
			/* core.gitproxy = none for kernel.org */
			if (matchlen == 4 &&
			    !memcmp(value, "none", 4))
				matchlen = 0;
			git_proxy_command = xmalloc(matchlen + 1);
			memcpy(git_proxy_command, value, matchlen);
			git_proxy_command[matchlen] = 0;
		}
		return 0;
	}

	return git_default_config(var, value);
}

static int git_use_proxy(const char *host)
{
	rhost_name = host;
	rhost_len = strlen(host);
	git_proxy_command = getenv("GIT_PROXY_COMMAND");
	git_config(git_proxy_command_options);
	rhost_name = NULL;
	return (git_proxy_command && *git_proxy_command);
}

static void git_proxy_connect(int fd[2], char *host)
{
	const char *port = STR(DEFAULT_GIT_PORT);
	char *colon, *end;
	const char *argv[4];
	struct child_process proxy;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
	}

	argv[0] = git_proxy_command;
	argv[1] = host;
	argv[2] = port;
	argv[3] = NULL;
	memset(&proxy, 0, sizeof(proxy));
	proxy.argv = argv;
	proxy.in = -1;
	proxy.out = -1;
	if (start_command(&proxy))
		die("cannot start proxy %s", argv[0]);
	fd[0] = proxy.out; /* read from proxy stdout */
	fd[1] = proxy.in;  /* write to proxy stdin */
}

#define MAX_CMD_LEN 1024

/*
 * This returns 0 if the transport protocol does not need fork(2),
 * or a process id if it does.  Once done, finish the connection
 * with finish_connect() with the value returned from this function
 * (it is safe to call finish_connect() with 0 to support the former
 * case).
 *
 * Does not return a negative value on error; it just dies.
 */
pid_t git_connect(int fd[2], char *url, const char *prog, int flags)
{
	char *host, *path = url;
	char *end;
	int c;
	int pipefd[2][2];
	pid_t pid;
	enum protocol protocol = PROTO_LOCAL;
	int free_path = 0;

	/* Without this we cannot rely on waitpid() to tell
	 * what happened to our children.
	 */
	signal(SIGCHLD, SIG_DFL);

	host = strstr(url, "://");
	if(host) {
		*host = '\0';
		protocol = get_protocol(url);
		host += 3;
		c = '/';
	} else {
		host = url;
		c = ':';
	}

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;

	path = strchr(end, c);
	if (c == ':') {
		if (path) {
			protocol = PROTO_SSH;
			*path++ = '\0';
		} else
			path = host;
	}

	if (!path || !*path)
		die("No path specified. See 'man git-pull' for valid url syntax");

	/*
	 * null-terminate hostname and point path to ~ for URL's like this:
	 *    ssh://host.xz/~user/repo
	 */
	if (protocol != PROTO_LOCAL && host != url) {
		char *ptr = path;
		if (path[1] == '~')
			path++;
		else {
			path = xstrdup(ptr);
			free_path = 1;
		}

		*ptr = '\0';
	}

	if (protocol == PROTO_GIT) {
		/* These underlying connection commands die() if they
		 * cannot connect.
		 */
		char *target_host = xstrdup(host);
		if (git_use_proxy(host))
			git_proxy_connect(fd, host);
		else
			git_tcp_connect(fd, host, flags);
		/*
		 * Separate original protocol components prog and path
		 * from extended components with a NUL byte.
		 */
		packet_write(fd[1],
			     "%s %s%chost=%s%c",
			     prog, path, 0,
			     target_host, 0);
		free(target_host);
		if (free_path)
			free(path);
		return 0;
	}

	if (pipe(pipefd[0]) < 0 || pipe(pipefd[1]) < 0)
		die("unable to create pipe pair for communication");
	pid = fork();
	if (pid < 0)
		die("unable to fork");
	if (!pid) {
		char command[MAX_CMD_LEN];
		char *posn = command;
		int size = MAX_CMD_LEN;
		int of = 0;

		of |= add_to_string(&posn, &size, prog, 0);
		of |= add_to_string(&posn, &size, " ", 0);
		of |= add_to_string(&posn, &size, path, 1);

		if (of)
			die("command line too long");

		dup2(pipefd[1][0], 0);
		dup2(pipefd[0][1], 1);
		close(pipefd[0][0]);
		close(pipefd[0][1]);
		close(pipefd[1][0]);
		close(pipefd[1][1]);
		if (protocol == PROTO_SSH) {
			const char *ssh, *ssh_basename;
			ssh = getenv("GIT_SSH");
			if (!ssh) ssh = "ssh";
			ssh_basename = strrchr(ssh, '/');
			if (!ssh_basename)
				ssh_basename = ssh;
			else
				ssh_basename++;
			execlp(ssh, ssh_basename, host, command, NULL);
		}
		else {
			unsetenv(ALTERNATE_DB_ENVIRONMENT);
			unsetenv(DB_ENVIRONMENT);
			unsetenv(GIT_DIR_ENVIRONMENT);
			unsetenv(GIT_WORK_TREE_ENVIRONMENT);
			unsetenv(GRAFT_ENVIRONMENT);
			unsetenv(INDEX_ENVIRONMENT);
			execlp("sh", "sh", "-c", command, NULL);
		}
		die("exec failed");
	}
	fd[0] = pipefd[0][0];
	fd[1] = pipefd[1][1];
	close(pipefd[0][1]);
	close(pipefd[1][0]);
	if (free_path)
		free(path);
	return pid;
}

int finish_connect(pid_t pid)
{
	if (pid == 0)
		return 0;

	while (waitpid(pid, NULL, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}
