/*
 * "git push"
 */
#include "cache.h"
#include "refs.h"
#include "run-command.h"
#include "builtin.h"
#include "remote.h"

static const char push_usage[] = "git-push [--all] [--tags] [--receive-pack=<git-receive-pack>] [--repo=all] [-f | --force] [-v] [<repository> <refspec>...]";

static int all, tags, force, thin = 1, verbose;
static const char *receivepack;

static const char **refspec;
static int refspec_nr;

static void add_refspec(const char *ref)
{
	int nr = refspec_nr + 1;
	refspec = xrealloc(refspec, nr * sizeof(char *));
	refspec[nr-1] = ref;
	refspec_nr = nr;
}

static int expand_one_ref(const char *ref, const unsigned char *sha1, int flag, void *cb_data)
{
	/* Ignore the "refs/" at the beginning of the refname */
	ref += 5;

	if (!prefixcmp(ref, "tags/"))
		add_refspec(xstrdup(ref));
	return 0;
}

static void expand_refspecs(void)
{
	if (all) {
		if (refspec_nr)
			die("cannot mix '--all' and a refspec");

		/*
		 * No need to expand "--all" - we'll just use
		 * the "--all" flag to send-pack
		 */
		return;
	}
	if (!tags)
		return;
	for_each_ref(expand_one_ref, NULL);
}

struct wildcard_cb {
	const char *from_prefix;
	int from_prefix_len;
	const char *to_prefix;
	int to_prefix_len;
	int force;
};

static int expand_wildcard_ref(const char *ref, const unsigned char *sha1, int flag, void *cb_data)
{
	struct wildcard_cb *cb = cb_data;
	int len = strlen(ref);
	char *expanded, *newref;

	if (len < cb->from_prefix_len ||
	    memcmp(cb->from_prefix, ref, cb->from_prefix_len))
		return 0;
	expanded = xmalloc(len * 2 + cb->force +
			   (cb->to_prefix_len - cb->from_prefix_len) + 2);
	newref = expanded + cb->force;
	if (cb->force)
		expanded[0] = '+';
	memcpy(newref, ref, len);
	newref[len] = ':';
	memcpy(newref + len + 1, cb->to_prefix, cb->to_prefix_len);
	strcpy(newref + len + 1 + cb->to_prefix_len,
	       ref + cb->from_prefix_len);
	add_refspec(expanded);
	return 0;
}

static int wildcard_ref(const char *ref)
{
	int len;
	const char *colon;
	struct wildcard_cb cb;

	memset(&cb, 0, sizeof(cb));
	if (ref[0] == '+') {
		cb.force = 1;
		ref++;
	}
	len = strlen(ref);
	colon = strchr(ref, ':');
	if (! (colon && ref < colon &&
	       colon[-2] == '/' && colon[-1] == '*' &&
	       /* "<mine>/<asterisk>:<yours>/<asterisk>" is at least 7 bytes */
	       7 <= len &&
	       ref[len-2] == '/' && ref[len-1] == '*') )
		return 0 ;
	cb.from_prefix = ref;
	cb.from_prefix_len = colon - ref - 1;
	cb.to_prefix = colon + 1;
	cb.to_prefix_len = len - (colon - ref) - 2;
	for_each_ref(expand_wildcard_ref, &cb);
	return 1;
}

static void set_refspecs(const char **refs, int nr)
{
	if (nr) {
		int i;
		for (i = 0; i < nr; i++) {
			const char *ref = refs[i];
			if (!strcmp("tag", ref)) {
				char *tag;
				int len;
				if (nr <= ++i)
					die("tag shorthand without <tag>");
				len = strlen(refs[i]) + 11;
				tag = xmalloc(len);
				strcpy(tag, "refs/tags/");
				strcat(tag, refs[i]);
				ref = tag;
			}
			else if (wildcard_ref(ref))
				continue;
			add_refspec(ref);
		}
	}
	expand_refspecs();
}

static int do_push(const char *repo)
{
	int i, errs;
	int common_argc;
	const char **argv;
	int argc;
	struct remote *remote = remote_get(repo);

	if (!remote)
		die("bad repository '%s'", repo);

	if (remote->receivepack) {
		char *rp = xmalloc(strlen(remote->receivepack) + 16);
		sprintf(rp, "--receive-pack=%s", remote->receivepack);
		receivepack = rp;
	}
	if (!refspec && !all && !tags && remote->push_refspec_nr) {
		for (i = 0; i < remote->push_refspec_nr; i++) {
			if (!wildcard_ref(remote->push_refspec[i]))
				add_refspec(remote->push_refspec[i]);
		}
	}

	argv = xmalloc((refspec_nr + 10) * sizeof(char *));
	argv[0] = "dummy-send-pack";
	argc = 1;
	if (all)
		argv[argc++] = "--all";
	if (force)
		argv[argc++] = "--force";
	if (receivepack)
		argv[argc++] = receivepack;
	common_argc = argc;

	errs = 0;
	for (i = 0; i < remote->uri_nr; i++) {
		int err;
		int dest_argc = common_argc;
		int dest_refspec_nr = refspec_nr;
		const char **dest_refspec = refspec;
		const char *dest = remote->uri[i];
		const char *sender = "send-pack";
		if (!prefixcmp(dest, "http://") ||
		    !prefixcmp(dest, "https://"))
			sender = "http-push";
		else {
			char *rem = xmalloc(strlen(remote->name) + 10);
			sprintf(rem, "--remote=%s", remote->name);
			argv[dest_argc++] = rem;
			if (thin)
				argv[dest_argc++] = "--thin";
		}
		argv[0] = sender;
		argv[dest_argc++] = dest;
		while (dest_refspec_nr--)
			argv[dest_argc++] = *dest_refspec++;
		argv[dest_argc] = NULL;
		if (verbose)
			fprintf(stderr, "Pushing to %s\n", dest);
		err = run_command_v_opt(argv, RUN_GIT_CMD);
		if (!err)
			continue;

		error("failed to push to '%s'", remote->uri[i]);
		switch (err) {
		case -ERR_RUN_COMMAND_FORK:
			error("unable to fork for %s", sender);
		case -ERR_RUN_COMMAND_EXEC:
			error("unable to exec %s", sender);
			break;
		case -ERR_RUN_COMMAND_WAITPID:
		case -ERR_RUN_COMMAND_WAITPID_WRONG_PID:
		case -ERR_RUN_COMMAND_WAITPID_SIGNAL:
		case -ERR_RUN_COMMAND_WAITPID_NOEXIT:
			error("%s died with strange error", sender);
		}
		errs++;
	}
	return !!errs;
}

int cmd_push(int argc, const char **argv, const char *prefix)
{
	int i;
	const char *repo = NULL;	/* default repository */

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (arg[0] != '-') {
			repo = arg;
			i++;
			break;
		}
		if (!strcmp(arg, "-v")) {
			verbose=1;
			continue;
		}
		if (!prefixcmp(arg, "--repo=")) {
			repo = arg+7;
			continue;
		}
		if (!strcmp(arg, "--all")) {
			all = 1;
			continue;
		}
		if (!strcmp(arg, "--tags")) {
			tags = 1;
			continue;
		}
		if (!strcmp(arg, "--force") || !strcmp(arg, "-f")) {
			force = 1;
			continue;
		}
		if (!strcmp(arg, "--thin")) {
			thin = 1;
			continue;
		}
		if (!strcmp(arg, "--no-thin")) {
			thin = 0;
			continue;
		}
		if (!prefixcmp(arg, "--receive-pack=")) {
			receivepack = arg;
			continue;
		}
		if (!prefixcmp(arg, "--exec=")) {
			receivepack = arg;
			continue;
		}
		usage(push_usage);
	}
	set_refspecs(argv + i, argc - i);
	return do_push(repo);
}
