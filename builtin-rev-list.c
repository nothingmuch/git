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
"    --bisect\n"
"    --bisect-vars"
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

static struct commit_list *find_bisection(struct commit_list *list,
					  int *reaches, int *all)
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
	*all = nr;
	closest = 0;
	best = list;

	for (p = list; p; p = p->next) {
		int distance, reach;

		if (revs.prune_fn && !(p->item->object.flags & TREECHANGE))
			continue;

		distance = reach = count_distance(p);
		clear_distance(list);
		if (nr - distance < distance)
			distance = nr - distance;
		if (distance > closest) {
			best = p;
			*reaches = reach;
			closest = distance;
		}
	}
	if (best)
		best->next = NULL;
	return best;
}

static inline int commit_interesting(struct commit_list *elem)
{
	unsigned flags = elem->item->object.flags;
	if (flags & UNINTERESTING)
		return 0;
	return (!revs.prune_fn || (flags & TREECHANGE));
}

static inline int weight(struct commit_list *elem)
{
	return *((int*)(elem->item->util));
}

static inline void weight_set(struct commit_list *elem, int weight)
{
	*((int*)(elem->item->util)) = weight;
}

static int count_interesting_parents(struct commit_list *elem)
{
	int cnt = 0;
	if (!elem->item->parents)
		return cnt;
	for (elem = elem->item->parents; elem; elem = elem->next) {
		if (commit_interesting(elem))
			cnt++;
	}
	return cnt;
}

static struct commit_list *find_bisection_2(struct commit_list *list,
					    int *reaches, int *all)
{
	int n, nr, counted, distance;
	struct commit_list *p, *best;
	int *weights;

	for (nr = 0, p = list; p; p = p->next) {
		if (commit_interesting(p))
			nr++;
	}
	*all = nr;
	weights = xcalloc(nr, sizeof(int*));
	counted = 0;

	for (n = 0, p = list; p; p = p->next) {
		if (!commit_interesting(p))
			continue;
		if (commit_interesting(p)) {
			/*
			 * positive weight is the number of interesting
			 * commits it can reach, including itself.
			 * weight = 0 means it has one parent and
			 * its distance is unknown.
			 * weight < 0 means it has more than one
			 * parent and its distance is unknown.
			 */
			p->item->util = &weights[n++];
			switch (count_interesting_parents(p)) {
			case 0:
				weight_set(p, 1);
				counted++;
				break;
			case 1:
				weight_set(p, 0);
				break;
			default:
				weight_set(p, -1);
				break;
			}
		}
	}

	/*
	 * If you have only one parent in the resulting set
	 * then you can reach one commit more than that parent
	 * can reach.  So we do not have to run the expensive
	 * count_distance() for single strand of pearls.
	 *
	 * However, if you have more than one parents, you cannot
	 * just add their distance and one for yourself, since
	 * they usually reach the same ancestor and you would
	 * end up counting them twice that way.
	 *
	 * So we will first count distance of merges the usual
	 * way, and then fill the blanks using cheaper algorithm.
	 */
	for (p = list; p; p = p->next) {
		if (!commit_interesting(p))
			continue;
		n = weight(p);
		if (0 <= n)
			continue;
		distance = count_distance(p);
		clear_distance(p);
		weight_set(p, distance);

		/* Does it happen to be at exactly half-way? */
		distance *= 2;
		if (nr == distance || (nr+1) == distance) {
			p->next = NULL;
			*reaches = weight(p);
			free(weights);
			return p;
		}
		counted++;
	}

	while (counted < nr) {
		for (p = list; p; p = p->next) {
			struct commit_list *q;

			if (!commit_interesting(p) || 0 < weight(p))
				continue;
			for (q = p->item->parents; q; q = q->next)
				if (commit_interesting(q) && 0 < weight(q))
					break;
			if (!q)
				continue;
			weight_set(p, weight(q)+1);
			counted++;

			/* Does it happen to be at exactly half-way? */
			distance = weight(p) * 2;
			if (nr == distance || (nr+1) == distance) {
				p->next = NULL;
				*reaches = weight(p);
				free(weights);
				return p;
			}
		}
	}

	/* Then find the best one */
	counted = 0;
	best = list;
	for (p = list; p; p = p->next) {
		if (!commit_interesting(p))
			continue;
		distance = weight(p);
		if (nr - distance < distance)
			distance = nr - distance;
		if (distance > counted) {
			best = p;
			counted = distance;
			*reaches = weight(p);
		}
	}
	if (best)
		best->next = NULL;
	free(weights);
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
	int bisect_show_vars = 0;

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
		if (!strcmp(arg, "--bisect-vars")) {
			bisect_list = 1;
			bisect_show_vars = 1;
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

	if (bisect_list) {
		int reaches = reaches, all = all;

		if (!revs.prune_fn)
			revs.commits = find_bisection_2(revs.commits,
							&reaches, &all);
		else
			revs.commits = find_bisection(revs.commits,
						      &reaches, &all);
		if (bisect_show_vars) {
			int cnt;
			if (!revs.commits)
				return 1;
			/*
			 * revs.commits can reach "reaches" commits among
			 * "all" commits.  If it is good, then there are
			 * (all-reaches) commits left to be bisected.
			 * On the other hand, if it is bad, then the set
			 * to bisect is "reaches".
			 * A bisect set of size N has (N-1) commits further
			 * to test, as we already know one bad one.
			 */
			cnt = all-reaches;
			if (cnt < reaches)
				cnt = reaches;
			printf("bisect_rev=%s\n"
			       "bisect_nr=%d\n"
			       "bisect_good=%d\n"
			       "bisect_bad=%d\n"
			       "bisect_all=%d\n",
			       sha1_to_hex(revs.commits->item->object.sha1),
			       cnt - 1,
			       all - reaches - 1,
			       reaches - 1,
			       all);
			return 0;
		}
	}

	traverse_commit_list(&revs, show_commit, show_object);

	return 0;
}
