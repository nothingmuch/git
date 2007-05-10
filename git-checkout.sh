#!/bin/sh

USAGE='[-q] [-f] [-b <new_branch>] [-m] [<branch>] [<paths>...]'
SUBDIRECTORY_OK=Sometimes
. git-sh-setup
require_work_tree

old_name=HEAD
old=$(git-rev-parse --verify $old_name 2>/dev/null)
oldbranch=$(git-symbolic-ref $old_name 2>/dev/null)
new=
new_name=
force=
branch=
track=
newbranch=
newbranch_log=
merge=
quiet=
v=-v
LF='
'
while [ "$#" != "0" ]; do
    arg="$1"
    shift
    case "$arg" in
	"-b")
		newbranch="$1"
		shift
		[ -z "$newbranch" ] &&
			die "git checkout: -b needs a branch name"
		git-show-ref --verify --quiet -- "refs/heads/$newbranch" &&
			die "git checkout: branch $newbranch already exists"
		git-check-ref-format "heads/$newbranch" ||
			die "git checkout: we do not like '$newbranch' as a branch name."
		;;
	"-l")
		newbranch_log=-l
		;;
	"--track"|"--no-track")
		track="$arg"
		;;
	"-f")
		force=1
		;;
	-m)
		merge=1
		;;
	"-q")
		quiet=1
		v=
		;;
	--)
		break
		;;
	-*)
		usage
		;;
	*)
		if rev=$(git-rev-parse --verify "$arg^0" 2>/dev/null)
		then
			if [ -z "$rev" ]; then
				echo "unknown flag $arg"
				exit 1
			fi
			new="$rev"
			new_name="$arg"
			if git-show-ref --verify --quiet -- "refs/heads/$arg"
			then
				branch="$arg"
			fi
		elif rev=$(git-rev-parse --verify "$arg^{tree}" 2>/dev/null)
		then
			# checking out selected paths from a tree-ish.
			new="$rev"
			new_name="$arg^{tree}"
			branch=
		else
			new=
			new_name=
			branch=
			set x "$arg" "$@"
			shift
		fi
		case "$1" in
		--)
			shift ;;
		esac
		break
		;;
    esac
done

case "$newbranch,$track" in
,--*)
	die "git checkout: --track and --no-track require -b"
esac

case "$force$merge" in
11)
	die "git checkout: -f and -m are incompatible"
esac

# The behaviour of the command with and without explicit path
# parameters is quite different.
#
# Without paths, we are checking out everything in the work tree,
# possibly switching branches.  This is the traditional behaviour.
#
# With paths, we are _never_ switching branch, but checking out
# the named paths from either index (when no rev is given),
# or the named tree-ish (when rev is given).

if test "$#" -ge 1
then
	hint=
	if test "$#" -eq 1
	then
		hint="
Did you intend to checkout '$@' which can not be resolved as commit?"
	fi
	if test '' != "$newbranch$force$merge"
	then
		die "git checkout: updating paths is incompatible with switching branches/forcing$hint"
	fi
	if test '' != "$new"
	then
		# from a specific tree-ish; note that this is for
		# rescuing paths and is never meant to remove what
		# is not in the named tree-ish.
		git-ls-tree --full-name -r "$new" "$@" |
		git-update-index --index-info || exit $?
	fi

	# Make sure the request is about existing paths.
	git-ls-files --error-unmatch -- "$@" >/dev/null || exit
	git-ls-files -- "$@" |
	git-checkout-index -f -u --stdin
	exit $?
else
	# Make sure we did not fall back on $arg^{tree} codepath
	# since we are not checking out from an arbitrary tree-ish,
	# but switching branches.
	if test '' != "$new"
	then
		git-rev-parse --verify "$new^{commit}" >/dev/null 2>&1 ||
		die "Cannot switch branch to a non-commit."
	fi
fi

# We are switching branches and checking out trees, so
# we *NEED* to be at the toplevel.
cd_to_toplevel

[ -z "$new" ] && new=$old && new_name="$old_name"

# If we don't have an existing branch that we're switching to,
# and we don't have a new branch name for the target we
# are switching to, then we are detaching our HEAD from any
# branch.  However, if "git checkout HEAD" detaches the HEAD
# from the current branch, even though that may be logically
# correct, it feels somewhat funny.  More importantly, we do not
# want "git checkout" nor "git checkout -f" to detach HEAD.

detached=
detach_warn=

describe_detached_head () {
	test -n "$quiet" || {
		printf >&2 "$1 "
		GIT_PAGER= git log >&2 -1 --pretty=oneline --abbrev-commit "$2"
	}
}

if test -z "$branch$newbranch" && test "$new_name" != "$old_name"
then
	detached="$new"
	if test -n "$oldbranch" && test -z "$quiet"
	then
		detach_warn="Note: moving to \"$new_name\" which isn't a local branch
If you want to create a new branch from this checkout, you may do so
(now or later) by using -b with the checkout command again. Example:
  git checkout -b <new_branch_name>"
	fi
elif test -z "$oldbranch" && test "$new" != "$old"
then
	describe_detached_head 'Previous HEAD position was' "$old"
fi

if [ "X$old" = X ]
then
	if test -z "$quiet"
	then
		echo >&2 "warning: You appear to be on a branch yet to be born."
		echo >&2 "warning: Forcing checkout of $new_name."
	fi
	force=1
fi

if [ "$force" ]
then
    git-read-tree $v --reset -u $new
else
    git-update-index --refresh >/dev/null
    merge_error=$(git-read-tree -m -u --exclude-per-directory=.gitignore $old $new 2>&1) || (
	case "$merge" in
	'')
		echo >&2 "$merge_error"
		exit 1 ;;
	esac

	# Match the index to the working tree, and do a three-way.
    	git diff-files --name-only | git update-index --remove --stdin &&
	work=`git write-tree` &&
	git read-tree $v --reset -u $new || exit

	eval GITHEAD_$new='${new_name:-${branch:-$new}}' &&
	eval GITHEAD_$work=local &&
	export GITHEAD_$new GITHEAD_$work &&
	git merge-recursive $old -- $new $work

	# Do not register the cleanly merged paths in the index yet.
	# this is not a real merge before committing, but just carrying
	# the working tree changes along.
	unmerged=`git ls-files -u`
	git read-tree $v --reset $new
	case "$unmerged" in
	'')	;;
	*)
		(
			z40=0000000000000000000000000000000000000000
			echo "$unmerged" |
			sed -e 's/^[0-7]* [0-9a-f]* /'"0 $z40 /"
			echo "$unmerged"
		) | git update-index --index-info
		;;
	esac
	exit 0
    )
    saved_err=$?
    if test "$saved_err" = 0 && test -z "$quiet"
    then
	git diff-index --name-status "$new"
    fi
    (exit $saved_err)
fi

# 
# Switch the HEAD pointer to the new branch if we
# checked out a branch head, and remove any potential
# old MERGE_HEAD's (subsequent commits will clearly not
# be based on them, since we re-set the index)
#
if [ "$?" -eq 0 ]; then
	if [ "$newbranch" ]; then
		git-branch $track $newbranch_log "$newbranch" "$new_name" || exit
		branch="$newbranch"
	fi
	if test -n "$branch"
	then
		GIT_DIR="$GIT_DIR" git-symbolic-ref -m "checkout: moving to $branch" HEAD "refs/heads/$branch"
		if test -n "$quiet"
		then
			true	# nothing
		elif test "refs/heads/$branch" = "$oldbranch"
		then
			echo >&2 "Already on branch \"$branch\""
		else
			echo >&2 "Switched to${newbranch:+ a new} branch \"$branch\""
		fi
	elif test -n "$detached"
	then
		git-update-ref --no-deref -m "checkout: moving to $arg" HEAD "$detached" ||
			die "Cannot detach HEAD"
		if test -n "$detach_warn"
		then
			echo >&2 "$detach_warn"
		fi
		describe_detached_head 'HEAD is now at' HEAD
	fi
	rm -f "$GIT_DIR/MERGE_HEAD"
else
	exit 1
fi
