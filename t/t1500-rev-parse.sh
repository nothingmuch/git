#!/bin/sh

test_description='test git rev-parse'
. ./test-lib.sh

test_rev_parse() {
	name=$1
	shift

	test_expect_success "$name: is-bare-repository" \
	"test '$1' = \"\$(git rev-parse --is-bare-repository)\""
	shift
	[ $# -eq 0 ] && return

	test_expect_success "$name: is-inside-git-dir" \
	"test '$1' = \"\$(git rev-parse --is-inside-git-dir)\""
	shift
	[ $# -eq 0 ] && return

	test_expect_success "$name: is-inside-work-tree" \
	"test '$1' = \"\$(git rev-parse --is-inside-work-tree)\""
	shift
	[ $# -eq 0 ] && return

	test_expect_success "$name: prefix" \
	"test '$1' = \"\$(git rev-parse --show-prefix)\""
	shift
	[ $# -eq 0 ] && return
}

test_rev_parse toplevel false false true ''

cd .git || exit 1
test_rev_parse .git/ false true true .git/
cd objects || exit 1
test_rev_parse .git/objects/ false true true .git/objects/
cd ../.. || exit 1

mkdir -p sub/dir || exit 1
cd sub/dir || exit 1
test_rev_parse subdirectory false false true sub/dir/
cd ../.. || exit 1

git config core.bare true
test_rev_parse 'core.bare = true' true false true

git config --unset core.bare
test_rev_parse 'core.bare undefined' false false true

mkdir work || exit 1
cd work || exit 1
export GIT_DIR=../.git
export GIT_CONFIG="$GIT_DIR"/config

git config core.bare false
test_rev_parse 'GIT_DIR=../.git, core.bare = false' false false true ''

git config core.bare true
test_rev_parse 'GIT_DIR=../.git, core.bare = true' true false true ''

git config --unset core.bare
test_rev_parse 'GIT_DIR=../.git, core.bare undefined' false false true ''

mv ../.git ../repo.git || exit 1
export GIT_DIR=../repo.git
export GIT_CONFIG="$GIT_DIR"/config

git config core.bare false
test_rev_parse 'GIT_DIR=../repo.git, core.bare = false' false false true ''

git config core.bare true
test_rev_parse 'GIT_DIR=../repo.git, core.bare = true' true false true ''

git config --unset core.bare
test_rev_parse 'GIT_DIR=../repo.git, core.bare undefined' true false true ''

test_done
