#!/bin/sh

test_description='git organize reconciles a tree against a declared layout'

. ./test-lib.sh

# A labeler records blob.c under odb, refs.c under refs, header.h as public.
# It writes a NUL-separated path and key=value label record per file.
write_labeler () {
	write_script labeler <<-\EOF
	printf 'blob.c\0component=odb role=lib\0'
	printf 'refs.c\0component=refs role=lib\0'
	printf 'header.h\0component=? role=public\0'
	EOF
}

# Point a stub organizer at the patch on its standard input; $2, if given, is
# the reason it declines a move.
write_organizer () {
	cat >"$1.patch" &&
	write_script "$1" <<-EOF
	cat >/dev/null
	printf 'git-organize 1 organize\n'
	${2:+printf 'reject %s\n' '$2'}
	printf 'patch\n'
	cat $1.patch
	EOF
}

# Point organize at the given labeler (and optional organizer), the way a
# project points a merge driver at a command, and write the project-authored
# [scope] pathspecs and [layout] rules: role:public and role:program keep a
# file at the root (first match wins), a component value routes to its directory.
configure_organize () {
	git config organize.labeler "$1" &&
	if test $# -gt 1
	then
		git config organize.organizer "$2"
	fi &&
	cat >.gitorganize <<-\EOF
	[scope]
	:(glob)*.c
	:(glob)*.h
	[layout]
	role:public = .
	role:program = .
	component:odb = odb
	component:refs = refs
	EOF
}

test_expect_success 'setup a tree and a labeler' '
	echo blob >blob.c &&
	echo refs >refs.c &&
	echo header >header.h &&
	git add . &&
	git commit -m init &&
	write_labeler &&
	configure_organize ./labeler &&
	git add .gitorganize &&
	git commit -m declare
'

test_expect_success 'apply --labels-only records the labels' '
	git organize apply --labels-only &&
	cat >expect <<-\EOF &&
	[scope]
	:(glob)*.c
	:(glob)*.h
	[layout]
	role:public = .
	role:program = .
	component:odb = odb
	component:refs = refs
	[labels]
	# git organize apply --labels-only regenerates the lines below.
	blob.c component=odb role=lib
	header.h component=? role=public
	refs.c component=refs role=lib
	EOF
	test_cmp expect .gitorganize &&
	git diff --cached --name-only >staged &&
	test_grep "^.gitorganize$" staged &&
	git commit -m labels
'

test_expect_success 'status reports the files to move' '
	git organize status >actual &&
	# header.h is public (in place at root); blob.c and refs.c move next
	test_grep "organize: 3 in scope (1 in place, 2 to move, 0 backlog)" actual &&
	test_grep "blob.c  *-> odb/blob.c" actual &&
	test_grep "refs.c  *-> refs/refs.c" actual &&
	test_grep ! header.h actual &&
	test_grep "2 file(s) would move" actual
'

test_expect_success 'status --exit-code fails when a file is out of place' '
	test_expect_code 1 git organize status --exit-code
'

test_expect_success 'status --label filters by a recorded label' '
	git organize status --label component=odb >actual &&
	test_grep "blob.c  *-> odb/blob.c" actual &&
	test_grep ! refs.c actual &&
	# a non-placement label selects too (role, not just the placing component)
	git organize status --label role=lib >bylib &&
	test_grep "blob.c  *-> odb/blob.c" bylib &&
	test_grep "refs.c  *-> refs/refs.c" bylib &&
	# header.h (role=public, in place) is not over-included by the filter
	test_grep ! "header.h" bylib &&
	# a bare key matches any value of that label
	git organize status --label component >bykey &&
	test_grep "blob.c  *-> odb/blob.c" bykey &&
	test_grep "refs.c  *-> refs/refs.c" bykey
'

test_expect_success 'apply moves files as content-identical renames and repoints [labels]' '
	git organize apply &&
	git diff --cached -M --name-status >actual &&
	test_grep "^R100.*blob.c.*odb/blob.c" actual &&
	test_grep "^R100.*refs.c.*refs/refs.c" actual &&
	test_path_is_file odb/blob.c &&
	test_path_is_file refs/refs.c &&
	test_path_is_file header.h &&
	test_path_is_missing blob.c &&
	test_path_is_missing refs.c &&
	git diff --cached --name-only >staged &&
	test_grep "^.gitorganize$" staged &&
	git commit -m reconciled &&
	git organize status --exit-code &&
	git organize status >actual &&
	test_grep "nothing to move" actual &&
	test_grep "^odb/blob.c component=odb" .gitorganize &&
	test_grep "^refs/refs.c component=refs" .gitorganize &&
	test_grep ! "^blob.c " .gitorganize &&
	test_grep ! "^refs.c " .gitorganize &&
	test_grep "^header.h component=? role=public" .gitorganize
'

test_expect_success 'apply refuses a dirty worktree' '
	git init dirty &&
	(
		cd dirty &&
		echo blob >blob.c &&
		echo other >other.c &&
		git add . &&
		git commit -m init &&
		write_labeler &&
		configure_organize ./labeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		echo dirty >>other.c &&
		test_must_fail git organize apply 2>err &&
		test_grep "uncommitted changes" err
	)
'

test_expect_success 'organizer may decline a move and patch a referrer' '
	git init sub &&
	(
		cd sub &&
		echo blob >blob.c &&
		echo refs >refs.c &&
		printf "blob.o\n" >build.list &&
		git add . &&
		git commit -m init &&
		write_labeler &&
		write_organizer organizer "refs.c held for test" <<-\EOF &&
		diff --git a/build.list b/build.list
		--- a/build.list
		+++ b/build.list
		@@ -1 +1 @@
		-blob.o
		+odb/blob.o
		EOF
		configure_organize ./labeler ./organizer &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		git organize apply >out &&
		test_grep "1 move(s), 1 skipped" out &&
		test_grep "skipped refs.c" out &&
		git diff --cached -M --name-status >actual &&
		test_grep "^R100.*blob.c.*odb/blob.c" actual &&
		test_grep "^M.*build.list" actual &&
		echo odb/blob.o >expect &&
		test_cmp expect build.list &&
		test_path_is_file refs.c &&
		test_path_is_file odb/blob.c &&
		test_path_is_missing blob.c &&
		# the carved file is repointed; the declined file keeps its line
		test_grep "^odb/blob.c component=odb" .gitorganize &&
		test_grep "^refs.c component=refs" .gitorganize &&
		test_grep ! "^blob.c " .gitorganize
	)
'

test_expect_success 'apply rejects an in-place edit of a moved file' '
	git init tamper &&
	(
		cd tamper &&
		echo blob >blob.c &&
		git add . &&
		git commit -m init &&
		write_labeler &&
		write_organizer tamper_organizer <<-\EOF &&
		diff --git a/blob.c b/blob.c
		--- a/blob.c
		+++ b/blob.c
		@@ -1 +1 @@
		-blob
		+tampered
		EOF
		configure_organize ./labeler ./tamper_organizer &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		test_must_fail git organize apply 2>err &&
		test_grep "part of a move" err &&
		# the rejected apply changed nothing
		test_path_is_file blob.c &&
		test_path_is_missing odb/blob.c
	)
'

test_expect_success 'apply rejects a rename that is not a planned move' '
	git init renamer &&
	(
		cd renamer &&
		echo blob >blob.c &&
		echo readme >README &&
		git add . &&
		git commit -m init &&
		write_labeler &&
		write_organizer rename_organizer <<-\EOF &&
		diff --git a/README b/MOVED
		similarity index 100%
		rename from README
		rename to MOVED
		EOF
		configure_organize ./labeler ./rename_organizer &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		test_must_fail git organize apply 2>err &&
		test_grep "not a planned move" err &&
		# the rejected apply changed nothing
		test_path_is_file README &&
		test_path_is_missing MOVED &&
		test_path_is_file blob.c
	)
'

test_expect_success 'apply rejects a body that edits a file the header does not name' '
	git init header-lies &&
	(
		cd header-lies &&
		echo blob >blob.c &&
		echo refs >refs.c &&
		git add . &&
		git commit -m init &&
		write_labeler &&
		# the diff --git header claims the planned blob.c move, but the body
		# edits refs.c (another moved file); apply resolves the real target
		# from the patch, not the header, and rejects it.
		write_organizer liar <<-\EOF &&
		diff --git a/blob.c b/odb/blob.c
		--- a/refs.c
		+++ b/refs.c
		@@ -1 +1 @@
		-refs
		+tampered
		EOF
		configure_organize ./labeler ./liar &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		test_must_fail git organize apply 2>err &&
		test_grep "part of a move" err &&
		# the rejected apply changed nothing
		test_grep "^refs$" refs.c &&
		test_path_is_missing refs/refs.c &&
		test_path_is_missing odb/blob.c
	)
'

test_expect_success 'organizer may rename-with-modification a moved file' '
	git init renamemod &&
	(
		cd renamemod &&
		cat >blob.c <<-\EOF &&
		#include "a.h"
		#include "b.h"
		#include "x.h"
		#include "c.h"
		#include "d.h"
		EOF
		git add . &&
		git commit -m init &&
		write_labeler &&
		write_organizer renamemod_organizer <<-\EOF &&
		diff --git a/blob.c b/odb/blob.c
		rename from blob.c
		rename to odb/blob.c
		--- a/blob.c
		+++ b/odb/blob.c
		@@ -1,5 +1,5 @@
		 #include "a.h"
		 #include "b.h"
		-#include "x.h"
		+#include "sub/x.h"
		 #include "c.h"
		 #include "d.h"
		EOF
		configure_organize ./labeler ./renamemod_organizer &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		git organize apply &&
		test_path_is_file odb/blob.c &&
		test_path_is_missing blob.c &&
		test_grep "sub/x.h" odb/blob.c &&
		git diff --cached -M --name-status >actual &&
		test_grep "^R[0-9]*.*blob.c.*odb/blob.c" actual &&
		test_grep "^odb/blob.c component=odb role=lib" .gitorganize &&
		test_grep ! "^blob.c " .gitorganize &&
		git diff --cached --name-only >staged &&
		test_grep "^.gitorganize$" staged
	)
'

test_expect_success 'a basename shared across directories does not collide' '
	git init dup &&
	(
		cd dup &&
		echo root >dup.c &&
		mkdir sub &&
		echo sub >sub/dup.c &&
		git add . &&
		git commit -m init &&
		# a labeler that labels only the root dup.c
		write_script duplabeler <<-\EOF &&
		printf "dup.c\0component=odb role=lib\0"
		EOF
		configure_organize ./duplabeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		git organize status >actual &&
		test_grep "dup.c  *-> odb/dup.c" actual &&
		test_grep ! "sub/dup.c" actual &&
		git organize apply &&
		test_path_is_file odb/dup.c &&
		test_path_is_file sub/dup.c &&
		test_path_is_missing dup.c &&
		git commit -m reconciled &&
		git organize status --exit-code
	)
'

test_expect_success 'a rule that maps a label to the root keeps its file in place' '
	git init prog &&
	(
		cd prog &&
		echo lib >lib.c &&
		echo tool >tool.c &&
		git add . &&
		git commit -m init &&
		# tool.c carries role=program, which the layout maps to the root
		# (role:program = .), so it stays; lib.c matches component:odb and moves
		write_script proglabeler <<-\EOF &&
		printf "lib.c\0component=odb role=lib\0"
		printf "tool.c\0component=? role=program\0"
		EOF
		configure_organize ./proglabeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		test_grep "^tool.c component=? role=program" .gitorganize &&
		git commit -m labels &&
		git organize status >actual &&
		test_grep "organize: 2 in scope (1 in place, 1 to move, 0 backlog)" actual &&
		test_grep ! "tool.c  *->" actual &&
		git organize apply &&
		test_path_is_file tool.c &&
		test_path_is_file odb/lib.c &&
		test_path_is_missing lib.c
	)
'

test_expect_success 'a file in scope with no recorded label is unrecorded' '
	git init scoped &&
	(
		cd scoped &&
		echo a >a.c &&
		git add . &&
		git commit -m init &&
		write_script alabeler <<-\EOF &&
		printf "a.c\0component=? role=lib\0"
		EOF
		configure_organize ./alabeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		# a.c is recorded but matches no rule: backlog, which does not redden
		git organize status >actual &&
		test_grep "backlog:" actual &&
		test_grep "^  a.c$" actual &&
		git organize status --exit-code &&
		# a source in scope that [labels] never recorded is unrecorded drift
		echo b >b.c &&
		git add b.c &&
		git commit -m add-b &&
		git organize status >actual &&
		test_grep "in scope but unrecorded:" actual &&
		test_grep "^  b.c$" actual &&
		test_expect_code 1 git organize status --exit-code
	)
'

test_expect_success 'status reports a recorded path that no longer exists' '
	git init orphan &&
	(
		cd orphan &&
		echo a >a.c &&
		echo b >b.c &&
		git add . &&
		git commit -m init &&
		write_script twolabeler <<-\EOF &&
		printf '\''a.c\0component=? role=public\0b.c\0component=? role=public\0'\''
		EOF
		configure_organize ./twolabeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		git commit -m labels &&
		# remove a recorded file outside the tool
		git rm -q b.c &&
		git commit -m drop-b &&
		git organize status >actual &&
		test_grep "declared but missing" actual &&
		test_grep "  b.c" actual &&
		test_expect_code 1 git organize status --exit-code
	)
'

test_expect_success 'status rejects a malformed .gitorganize' '
	git init bad &&
	(
		cd bad &&
		echo a >a.c &&
		git add . &&
		git commit -m init &&
		cat >.gitorganize <<-\EOF &&
		[layout]
		component:odb = odb
		[labels]
		a.c component=odb
		a.c component=odb
		EOF
		test_must_fail git organize status 2>err &&
		test_grep "listed twice" err &&
		cat >.gitorganize <<-\EOF &&
		[layout]
		nocolon = .
		EOF
		test_must_fail git organize status 2>err &&
		test_grep "label:value = directory" err &&
		cat >.gitorganize <<-\EOF &&
		[layout]
		component:x = ../evil
		EOF
		test_must_fail git organize status 2>err &&
		test_grep "must be inside the tree" err &&
		cat >.gitorganize <<-\EOF &&
		stray line
		EOF
		test_must_fail git organize status 2>err &&
		test_grep "line outside" err
	)
'

test_expect_success 'the recorded labels carry arbitrary key=value pairs' '
	git init extra &&
	(
		cd extra &&
		echo z >z.c &&
		git add . &&
		git commit -m init &&
		write_script extralabeler <<-\EOF &&
		printf '\''z.c\0component=? role=lib note=demo\0'\''
		EOF
		configure_organize ./extralabeler &&
		git add .gitorganize &&
		git commit -m declare &&
		git organize apply --labels-only &&
		# a label the engine does not place still round-trips into [labels]
		test_grep "^z.c component=? role=lib note=demo" .gitorganize
	)
'

test_expect_success 'a subcommand rejects extra operands' '
	test_must_fail git -C bad organize status junk 2>err &&
	test_grep "too many arguments" err
'

test_done
