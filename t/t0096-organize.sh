#!/bin/sh

test_description='git organize reconciles a tree against a declared layout'

. ./test-lib.sh

# There is no labeler yet, so tests write .gitorganize by hand: root *.c/*.h in
# scope, odb/refs components, and a [labels] census.
write_declaration () {
	cat >.gitorganize <<-\EOF
	[scope]
	:(glob)*.c
	:(glob)*.h
	[layout]
	role:public = .
	role:program = .
	component:odb = odb
	component:refs = refs
	[labels]
	blob.c component=odb role=lib
	header.h component=? role=public
	refs.c component=refs role=lib
	EOF
}

test_expect_success 'setup a tree and a declaration' '
	echo blob >blob.c &&
	echo refs >refs.c &&
	echo header >header.h &&
	git add . &&
	git commit -m init &&
	write_declaration &&
	git add .gitorganize &&
	git commit -m declare
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
		cat >.gitorganize <<-\EOF &&
		[scope]
		:(glob)*.c
		[layout]
		component:odb = odb
		[labels]
		blob.c component=odb
		other.c component=?
		EOF
		git add .gitorganize &&
		git commit -m declare &&
		echo dirty >>other.c &&
		test_must_fail git organize apply 2>err &&
		test_grep "uncommitted changes" err
	)
'

test_expect_success 'a file in scope with no matching rule is backlog' '
	git init backlog &&
	(
		cd backlog &&
		echo a >a.c &&
		git add . &&
		git commit -m init &&
		cat >.gitorganize <<-\EOF &&
		[scope]
		:(glob)*.c
		[layout]
		component:odb = odb
		[labels]
		a.c component=?
		EOF
		git add .gitorganize &&
		git commit -m declare &&
		git organize status >actual &&
		test_grep "backlog" actual &&
		test_grep "^  a.c$" actual
	)
'

test_expect_success 'status reports a declared path that no longer exists' '
	git init orphan &&
	(
		cd orphan &&
		echo a >a.c &&
		echo b >b.c &&
		git add . &&
		git commit -m init &&
		cat >.gitorganize <<-\EOF &&
		[scope]
		:(glob)*.c
		[layout]
		role:public = .
		[labels]
		a.c role=public
		b.c role=public
		EOF
		git add .gitorganize &&
		git commit -m declare &&
		git rm -q b.c &&
		git commit -m drop-b &&
		git organize status >actual &&
		test_grep "declared but missing" actual &&
		test_grep "  b.c" actual
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

test_expect_success 'a subcommand rejects extra operands' '
	test_must_fail git -C bad organize status junk 2>err &&
	test_grep "too many arguments" err
'

test_done
