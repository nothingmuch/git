#include "cache.h"
#include "attr.h"

/*
 * convert.c - convert a file when checking it out and checking it in.
 *
 * This should use the pathname to decide on whether it wants to do some
 * more interesting conversions (automatic gzip/unzip, general format
 * conversions etc etc), but by default it just does automatic CRLF<->LF
 * translation when the "auto_crlf" option is set.
 */

struct text_stat {
	/* CR, LF and CRLF counts */
	unsigned cr, lf, crlf;

	/* These are just approximations! */
	unsigned printable, nonprintable;
};

static void gather_stats(const char *buf, unsigned long size, struct text_stat *stats)
{
	unsigned long i;

	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < size; i++) {
		unsigned char c = buf[i];
		if (c == '\r') {
			stats->cr++;
			if (i+1 < size && buf[i+1] == '\n')
				stats->crlf++;
			continue;
		}
		if (c == '\n') {
			stats->lf++;
			continue;
		}
		if (c == 127)
			/* DEL */
			stats->nonprintable++;
		else if (c < 32) {
			switch (c) {
				/* BS, HT, ESC and FF */
			case '\b': case '\t': case '\033': case '\014':
				stats->printable++;
				break;
			default:
				stats->nonprintable++;
			}
		}
		else
			stats->printable++;
	}
}

/*
 * The same heuristics as diff.c::mmfile_is_binary()
 */
static int is_binary(unsigned long size, struct text_stat *stats)
{

	if ((stats->printable >> 7) < stats->nonprintable)
		return 1;
	/*
	 * Other heuristics? Average line length might be relevant,
	 * as might LF vs CR vs CRLF counts..
	 *
	 * NOTE! It might be normal to have a low ratio of CRLF to LF
	 * (somebody starts with a LF-only file and edits it with an editor
	 * that adds CRLF only to lines that are added..). But do  we
	 * want to support CR-only? Probably not.
	 */
	return 0;
}

static int autocrlf_to_git(const char *path, char **bufp, unsigned long *sizep)
{
	char *buffer, *nbuf;
	unsigned long size, nsize;
	struct text_stat stats;

	if (!auto_crlf)
		return 0;

	size = *sizep;
	if (!size)
		return 0;
	buffer = *bufp;

	gather_stats(buffer, size, &stats);

	/* No CR? Nothing to convert, regardless. */
	if (!stats.cr)
		return 0;

	/*
	 * We're currently not going to even try to convert stuff
	 * that has bare CR characters. Does anybody do that crazy
	 * stuff?
	 */
	if (stats.cr != stats.crlf)
		return 0;

	/*
	 * And add some heuristics for binary vs text, of course...
	 */
	if (is_binary(size, &stats))
		return 0;

	/*
	 * Ok, allocate a new buffer, fill it in, and return true
	 * to let the caller know that we switched buffers on it.
	 */
	nsize = size - stats.crlf;
	nbuf = xmalloc(nsize);
	*bufp = nbuf;
	*sizep = nsize;
	do {
		unsigned char c = *buffer++;
		if (c != '\r')
			*nbuf++ = c;
	} while (--size);

	return 1;
}

static int autocrlf_to_working_tree(const char *path, char **bufp, unsigned long *sizep)
{
	char *buffer, *nbuf;
	unsigned long size, nsize;
	struct text_stat stats;
	unsigned char last;

	/*
	 * FIXME! Other pluggable conversions should go here,
	 * based on filename patterns. Right now we just do the
	 * stupid auto-CRLF one.
	 */
	if (auto_crlf <= 0)
		return 0;

	size = *sizep;
	if (!size)
		return 0;
	buffer = *bufp;

	gather_stats(buffer, size, &stats);

	/* No LF? Nothing to convert, regardless. */
	if (!stats.lf)
		return 0;

	/* Was it already in CRLF format? */
	if (stats.lf == stats.crlf)
		return 0;

	/* If we have any bare CR characters, we're not going to touch it */
	if (stats.cr != stats.crlf)
		return 0;

	if (is_binary(size, &stats))
		return 0;

	/*
	 * Ok, allocate a new buffer, fill it in, and return true
	 * to let the caller know that we switched buffers on it.
	 */
	nsize = size + stats.lf - stats.crlf;
	nbuf = xmalloc(nsize);
	*bufp = nbuf;
	*sizep = nsize;
	last = 0;
	do {
		unsigned char c = *buffer++;
		if (c == '\n' && last != '\r')
			*nbuf++ = '\r';
		*nbuf++ = c;
		last = c;
	} while (--size);

	return 1;
}

static void setup_crlf_check(struct git_attr_check *check)
{
	static struct git_attr *attr_crlf;

	if (!attr_crlf)
		attr_crlf = git_attr("crlf", 4);
	check->attr = attr_crlf;
}

static int git_path_is_binary(const char *path)
{
	struct git_attr_check attr_crlf_check;

	setup_crlf_check(&attr_crlf_check);

	/*
	 * If crlf is not mentioned, default to autocrlf;
	 * disable autocrlf only when crlf attribute is explicitly
	 * unset.
	 */
	return (!git_checkattr(path, 1, &attr_crlf_check) &&
		(0 == attr_crlf_check.isset));
}

int convert_to_git(const char *path, char **bufp, unsigned long *sizep)
{
	if (git_path_is_binary(path))
		return 0;
	return autocrlf_to_git(path, bufp, sizep);
}

int convert_to_working_tree(const char *path, char **bufp, unsigned long *sizep)
{
	if (git_path_is_binary(path))
		return 0;
	return autocrlf_to_working_tree(path, bufp, sizep);
}
