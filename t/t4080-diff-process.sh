#!/bin/sh

test_description='diff.<driver>.process: oid-only hunk requests'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

# See t/helper/test-diff-process-backend.c for the process implementation
# and available --mode= options.

BACKEND="test-tool diff-process-backend"

test_expect_success 'setup' '
	echo "*.c diff=cdiff" >.gitattributes &&
	git add .gitattributes &&

	# 10 lines, changes at 5-6 and 9-10 between the two commits.
	cat >pair.c <<-\EOF &&
	line1
	line2
	line3
	line4
	original5
	original6
	line7
	line8
	line9
	line10
	EOF
	git add pair.c &&
	git commit -m "add pair.c" &&

	cat >pair.c <<-\EOF &&
	line1
	line2
	line3
	line4
	changed5
	changed6
	line7
	line8
	changed9
	changed10
	EOF
	git add pair.c &&
	git commit -m "change pair.c"
'

test_expect_success 'an oid-capable process answers blame by object names alone' '
	test_when_finished "rm -f backend.log" &&
	ORIG=$(git rev-parse --short HEAD~1) &&
	CHANGE=$(git rev-parse --short HEAD) &&
	# The process reports only lines 5-6 as changed, so blame attributes
	# lines 9-10 to the original commit even though the builtin diff
	# would show them as changed.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame pair.c >actual &&
	sed -n "9p" actual >line9 &&
	sed -n "10p" actual >line10 &&
	test_grep "$ORIG" line9 &&
	test_grep "$ORIG" line10 &&
	sed -n "5p" actual >line5 &&
	test_grep "$CHANGE" line5 &&
	test_grep "command=hunks-by-oid pathname=pair.c" backend.log
'

test_expect_success 'an oid-capable process answers --numstat by object names alone' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -1 --format= --numstat -- pair.c >actual &&
	printf "2\t2\tpair.c\n" >expect &&
	test_cmp expect actual &&
	test_grep "command=hunks-by-oid pathname=pair.c" backend.log
'

test_expect_success 'need-content is a miss: the builtin diff answers' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-need-content --log=backend.log" \
		log -1 --format= --numstat -- pair.c >actual &&
	printf "4\t4\tpair.c\n" >expect &&
	test_cmp expect actual &&
	test_grep "command=hunks-by-oid pathname=pair.c" backend.log
'

test_expect_success 'a warmed hunk store does not override process hunks' '
	test_when_finished "git diff-hunks clear" &&
	ORIG=$(git rev-parse --short HEAD~1) &&
	GIT_DIFF_HUNKS_WRITE=1 git log -2 --stat -- pair.c >/dev/null &&

	# Control: without a process, blame is served from the store.
	git blame --show-stats pair.c >stats &&
	test_grep "num precomputed hits: 1" stats &&

	# The store holds the builtin hunks, but a process-capable driver
	# makes the process the producer, so blame must reflect the process
	# hunks (only lines 5-6), not a store hit.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed" \
		blame pair.c >actual &&
	sed -n "9p" actual >line9 &&
	test_grep "$ORIG" line9
'

test_expect_success 'a worktree side is not asked by object names' '
	test_when_finished "rm -f backend.log && git checkout -- pair.c" &&
	echo "worktree edit" >>pair.c &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		diff --numstat -- pair.c >actual &&
	printf "1\t0\tpair.c\n" >expect &&
	test_cmp expect actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process bypassed by --no-ext-diff' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -1 --format= --numstat --no-ext-diff -- pair.c >actual &&
	printf "4\t4\tpair.c\n" >expect &&
	test_cmp expect actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process not used by format-patch' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		format-patch -1 --stdout -- pair.c >actual &&
	test_grep "^+changed9" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process not used by format-patch --ext-diff' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		format-patch -1 --stdout --ext-diff -- pair.c >actual &&
	test_grep "^+changed9" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process not consulted by plumbing diff commands' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		diff-tree --numstat HEAD >actual &&
	test_grep "pair.c" actual &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		diff-index --numstat HEAD -- pair.c >actual &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		diff-files --numstat -- pair.c >actual &&
	test_path_is_missing backend.log
'

test_expect_success 'add -p stages from the builtin diff with a process configured' '
	test_when_finished "rm -f backend.log" &&
	cat >gate.c <<-\EOF &&
	int gate(void) { return 1; }
	EOF
	git add gate.c &&
	git commit -m "add gate.c" &&
	cat >gate.c <<-\EOF &&
	int gate(void) { return 2; }
	EOF
	# A configured process must not shape what add -p offers: the
	# hunk-collecting child is plumbing, so it is not consulted.
	test_write_lines y |
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		add -p gate.c &&
	git diff --cached -- gate.c >staged &&
	test_grep "return 2" staged &&
	test_path_is_missing backend.log &&
	git commit -m "gate.c v2"
'

test_expect_success 'range-diff output is not shaped by the diff process' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		range-diff HEAD~2..HEAD~1 HEAD~1..HEAD >actual &&
	test_path_is_missing backend.log
'

test_expect_success 'blame withholds identity for the working-tree pair' '
	test_when_finished "rm -f backend.log && git checkout -- pair.c" &&
	echo "uncommitted" >>pair.c &&
	wt=$(git hash-object pair.c) &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame pair.c >actual &&
	# The dirty working-tree side is not a stored blob: no request
	# may name its bytes by object id.
	test_grep ! "new-oid=$wt" backend.log
'

test_done
