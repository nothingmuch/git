#!/bin/sh
#
# Copyright (c) 2005 Linus Torvalds
#

USAGE='[-a] [-d] [-f] [-l] [-n] [-q] [--max-pack-size=N] [--window=N] [--depth=N]'
SUBDIRECTORY_OK='Yes'
. git-sh-setup

no_update_info= all_into_one= remove_redundant=
local= quiet= no_reuse= extra=
while case "$#" in 0) break ;; esac
do
	case "$1" in
	-n)	no_update_info=t ;;
	-a)	all_into_one=t ;;
	-d)	remove_redundant=t ;;
	-q)	quiet=-q ;;
	-f)	no_reuse=--no-reuse-object ;;
	-l)	local=--local ;;
	--max-pack-size=*) extra="$extra $1" ;;
	--window=*) extra="$extra $1" ;;
	--depth=*) extra="$extra $1" ;;
	*)	usage ;;
	esac
	shift
done

# Later we will default repack.UseDeltaBaseOffset to true
default_dbo=false

case "`git config --bool repack.usedeltabaseoffset ||
       echo $default_dbo`" in
true)
	extra="$extra --delta-base-offset" ;;
esac

PACKDIR="$GIT_OBJECT_DIRECTORY/pack"
PACKTMP="$GIT_OBJECT_DIRECTORY/.tmp-$$-pack"
rm -f "$PACKTMP"-*
trap 'rm -f "$PACKTMP"-*' 0 1 2 3 15

# There will be more repacking strategies to come...
case ",$all_into_one," in
,,)
	args='--unpacked --incremental'
	;;
,t,)
	if [ -d "$PACKDIR" ]; then
		for e in `cd "$PACKDIR" && find . -type f -name '*.pack' \
			| sed -e 's/^\.\///' -e 's/\.pack$//'`
		do
			if [ -e "$PACKDIR/$e.keep" ]; then
				: keep
			else
				args="$args --unpacked=$e.pack"
				existing="$existing $e"
			fi
		done
	fi
	[ -z "$args" ] && args='--unpacked --incremental'
	;;
esac

args="$args $local $quiet $no_reuse$extra"
names=$(git-pack-objects --non-empty --all --reflog $args </dev/null "$PACKTMP") ||
	exit 1
if [ -z "$names" ]; then
	echo Nothing new to pack.
fi
for name in $names ; do
	chmod a-w "$PACKTMP-$name.pack"
	chmod a-w "$PACKTMP-$name.idx"
	if test "$quiet" != '-q'; then
	    echo "Pack pack-$name created."
	fi
	mkdir -p "$PACKDIR" || exit

	for sfx in pack idx
	do
		if test -f "$PACKDIR/pack-$name.$sfx"
		then
			mv -f "$PACKDIR/pack-$name.$sfx" \
				"$PACKDIR/old-pack-$name.$sfx"
		fi
	done &&
	mv -f "$PACKTMP-$name.pack" "$PACKDIR/pack-$name.pack" &&
	mv -f "$PACKTMP-$name.idx"  "$PACKDIR/pack-$name.idx" &&
	test -f "$PACKDIR/pack-$name.pack" &&
	test -f "$PACKDIR/pack-$name.idx" || {
		echo >&2 "Couldn't replace the existing pack with updated one."
		echo >&2 "The original set of packs have been saved as"
		echo >&2 "old-pack-$name.{pack,idx} in $PACKDIR."
		exit 1
	}
	rm -f "$PACKDIR/old-pack-$name.pack" "$PACKDIR/old-pack-$name.idx"
done

if test "$remove_redundant" = t
then
	# We know $existing are all redundant.
	if [ -n "$existing" ]
	then
		sync
		( cd "$PACKDIR" &&
		  for e in $existing
		  do
			case "$e" in
			pack-$name) ;;
			*)	rm -f "$e.pack" "$e.idx" "$e.keep" ;;
			esac
		  done
		)
	fi
	git-prune-packed $quiet
fi

case "$no_update_info" in
t) : ;;
*) git-update-server-info ;;
esac
