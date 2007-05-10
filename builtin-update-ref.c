#include "cache.h"
#include "refs.h"
#include "builtin.h"

static const char git_update_ref_usage[] =
"git-update-ref [-m <reason>] (-d <refname> <value> | [--no-deref] <refname> <value> [<oldval>])";

int cmd_update_ref(int argc, const char **argv, const char *prefix)
{
	const char *refname=NULL, *value=NULL, *oldval=NULL, *msg=NULL;
	struct ref_lock *lock;
	unsigned char sha1[20], oldsha1[20];
	int i, delete, ref_flags;

	delete = 0;
	ref_flags = 0;
	git_config(git_default_config);

	for (i = 1; i < argc; i++) {
		if (!strcmp("-m", argv[i])) {
			if (i+1 >= argc)
				usage(git_update_ref_usage);
			msg = argv[++i];
			if (!*msg)
				die("Refusing to perform update with empty message.");
			if (strchr(msg, '\n'))
				die("Refusing to perform update with \\n in message.");
			continue;
		}
		if (!strcmp("-d", argv[i])) {
			delete = 1;
			continue;
		}
		if (!strcmp("--no-deref", argv[i])) {
			ref_flags |= REF_NODEREF;
			continue;
		}
		if (!refname) {
			refname = argv[i];
			continue;
		}
		if (!value) {
			value = argv[i];
			continue;
		}
		if (!oldval) {
			oldval = argv[i];
			continue;
		}
	}
	if (!refname || !value)
		usage(git_update_ref_usage);

	if (get_sha1(value, sha1))
		die("%s: not a valid SHA1", value);

	if (delete) {
		if (oldval)
			usage(git_update_ref_usage);
		return delete_ref(refname, sha1);
	}

	hashclr(oldsha1);
	if (oldval && *oldval && get_sha1(oldval, oldsha1))
		die("%s: not a valid old SHA1", oldval);

	lock = lock_any_ref_for_update(refname, oldval ? oldsha1 : NULL, ref_flags);
	if (!lock)
		die("%s: cannot lock the ref", refname);
	if (write_ref_sha1(lock, sha1, msg) < 0)
		die("%s: cannot update the ref", refname);
	return 0;
}
