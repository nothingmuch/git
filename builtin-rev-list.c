#include "cache.h"
#include "refs.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"
#include "tree-walk.h"
#include "diff.h"
#include "revision.h"
#include "list-objects.h"
#include "builtin.h"

/* bits #0-15 in revision.h */

#define COUNTED		(1u<<16)

static const char rev_list_usage[] =
"git-rev-list [OPTION] <commit-id>... [ -- paths... ]\n"
"  limiting output:\n"
"    --max-count=nr\n"
"    --max-age=epoch\n"
"    --min-age=epoch\n"
"    --sparse\n"
"    --no-merges\n"
"    --remove-empty\n"
"    --all\n"
"    --stdin\n"
"  ordering output:\n"
"    --topo-order\n"
"    --date-order\n"
"  formatting output:\n"
"    --parents\n"
"    --objects | --objects-edge\n"
"    --unpacked\n"
"    --header | --pretty\n"
"    --abbrev=nr | --no-abbrev\n"
"    --abbrev-commit\n"
"  special purpose:\n"
"    --bisect"
;

static struct rev_info revs;

static int bisect_list;
static int show_timestamp;
static int hdr_termination;
static const char *header_prefix;

static void show_commit(struct commit *commit)
{
	if (show_timestamp)
		printf("%lu ", commit->date);
	if (header_prefix)
		fputs(header_prefix, stdout);
	if (commit->object.flags & BOUNDARY)
		putchar('-');
	else if (revs.left_right) {
		if (commit->object.flags & SYMMETRIC_LEFT)
			putchar('<');
		else
			putchar('>');
	}
	if (revs.abbrev_commit && revs.abbrev)
		fputs(find_unique_abbrev(commit->object.sha1, revs.abbrev),
		      stdout);
	else
		fputs(sha1_to_hex(commit->object.sha1), stdout);
	if (revs.parents) {
		struct commit_list *parents = commit->parents;
		while (parents) {
			struct object *o = &(parents->item->object);
			parents = parents->next;
			if (o->flags & TMP_MARK)
				continue;
			printf(" %s", sha1_to_hex(o->sha1));
			o->flags |= TMP_MARK;
		}
		/* TMP_MARK is a general purpose flag that can
		 * be used locally, but the user should clean
		 * things up after it is done with them.
		 */
		for (parents = commit->parents;
		     parents;
		     parents = parents->next)
			parents->item->object.flags &= ~TMP_MARK;
	}
	if (revs.commit_format == CMIT_FMT_ONELINE)
		putchar(' ');
	else
		putchar('\n');

	if (revs.verbose_header) {
		static char pretty_header[16384];
		pretty_print_commit(revs.commit_format, commit, ~0,
				    pretty_header, sizeof(pretty_header),
				    revs.abbrev, NULL, NULL, revs.relative_date);
		printf("%s%c", pretty_header, hdr_termination);
	}
	fflush(stdout);
	if (commit->parents) {
		free_commit_list(commit->parents);
		commit->parents = NULL;
	}
	free(commit->buffer);
	commit->buffer = NULL;
}

static void show_object(struct object_array_entry *p)
{
	/* An object with name "foo\n0000000..." can be used to
	 * confuse downstream git-pack-objects very badly.
	 */
	const char *ep = strchr(p->name, '\n');
	if (ep) {
		printf("%s %.*s\n", sha1_to_hex(p->item->sha1),
		       (int) (ep - p->name),
		       p->name);
	}
	else
		printf("%s %s\n", sha1_to_hex(p->item->sha1), p->name);
}

static void show_edge(struct commit *commit)
{
	printf("-%s\n", sha1_to_hex(commit->object.sha1));
}

/*
 * This is a truly stupid algorithm, but it's only
 * used for bisection, and we just don't care enough.
 *
 * We care just barely enough to avoid recursing for
 * non-merge entries.
 */
static int count_distance(struct commit_list *entry)
{
	int nr = 0;

	while (entry) {
		struct commit *commit = entry->item;
		struct commit_list *p;

		if (commit->object.flags & (UNINTERESTING | COUNTED))
			break;
		if (!revs.prune_fn || (commit->object.flags & TREECHANGE))
			nr++;
		commit->object.flags |= COUNTED;
		p = commit->parents;
		entry = p;
		if (p) {
			p = p->next;
			while (p) {
				nr += count_distance(p);
				p = p->next;
			}
		}
	}

	return nr;
}

static void clear_distance(struct commit_list *list)
{
	while (list) {
		struct commit *commit = list->item;
		commit->object.flags &= ~COUNTED;
		list = list->next;
	}
}

static struct commit_list *find_bisection(struct commit_list *list)
{
	int nr, closest;
	struct commit_list *p, *best;

	nr = 0;
	p = list;
	while (p) {
		if (!revs.prune_fn || (p->item->object.flags & TREECHANGE))
			nr++;
		p = p->next;
	}
	closest = 0;
	best = list;

	for (p = list; p; p = p->next) {
		int distance;

		if (revs.prune_fn && !(p->item->object.flags & TREECHANGE))
			continue;

		distance = count_distance(p);
		clear_distance(list);
		if (nr - distance < distance)
			distance = nr - distance;
		if (distance > closest) {
			best = p;
			closest = distance;
		}
	}
	if (best)
		best->next = NULL;
	return best;
}

static void read_revisions_from_stdin(struct rev_info *revs)
{
	char line[1000];

	while (fgets(line, sizeof(line), stdin) != NULL) {
		int len = strlen(line);
		if (line[len - 1] == '\n')
			line[--len] = 0;
		if (!len)
			break;
		if (line[0] == '-')
			die("options not supported in --stdin mode");
		if (handle_revision_arg(line, revs, 0, 1))
			die("bad revision '%s'", line);
	}
}

int cmd_rev_list(int argc, const char **argv, const char *prefix)
{
	struct commit_list *list;
	int i;
	int read_from_stdin = 0;

	git_config(git_default_config);
	init_revisions(&revs, prefix);
	revs.abbrev = 0;
	revs.commit_format = CMIT_FMT_UNSPECIFIED;
	argc = setup_revisions(argc, argv, &revs, NULL);

	for (i = 1 ; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--header")) {
			revs.verbose_header = 1;
			continue;
		}
		if (!strcmp(arg, "--timestamp")) {
			show_timestamp = 1;
			continue;
		}
		if (!strcmp(arg, "--bisect")) {
			bisect_list = 1;
			continue;
		}
		if (!strcmp(arg, "--stdin")) {
			if (read_from_stdin++)
				die("--stdin given twice?");
			read_revisions_from_stdin(&revs);
			continue;
		}
		usage(rev_list_usage);

	}
	if (revs.commit_format != CMIT_FMT_UNSPECIFIED) {
		/* The command line has a --pretty  */
		hdr_termination = '\n';
		if (revs.commit_format == CMIT_FMT_ONELINE)
			header_prefix = "";
		else
			header_prefix = "commit ";
	}
	else if (revs.verbose_header)
		/* Only --header was specified */
		revs.commit_format = CMIT_FMT_RAW;

	list = revs.commits;

	if ((!list &&
	     (!(revs.tag_objects||revs.tree_objects||revs.blob_objects) &&
	      !revs.pending.nr)) ||
	    revs.diff)
		usage(rev_list_usage);

	save_commit_buffer = revs.verbose_header || revs.grep_filter;
	track_object_refs = 0;
	if (bisect_list)
		revs.limited = 1;

	prepare_revision_walk(&revs);
	if (revs.tree_objects)
		mark_edges_uninteresting(revs.commits, &revs, show_edge);

	if (bisect_list)
		revs.commits = find_bisection(revs.commits);

	traverse_commit_list(&revs, show_commit, show_object);

	return 0;
}
