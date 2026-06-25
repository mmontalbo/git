#!/bin/sh

test_description='diff process via long-running process'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

# See t/helper/test-diff-process-backend.c for the backend implementation
# and available --mode= options.

BACKEND="test-tool diff-process-backend"

test_expect_success 'setup' '
	echo "*.c diff=cdiff" >.gitattributes &&
	git add .gitattributes &&

	# boundary.c: 10 lines, changes at 5-6 and 9-10.
	cat >boundary.c <<-\EOF &&
	line1
	line2
	line3
	line4
	OLD5
	OLD6
	line7
	line8
	OLD9
	OLD10
	EOF
	git add boundary.c &&

	# worddiff.c: single-line function, value changes 1 -> 999.
	cat >worddiff.c <<-\EOF &&
	int value(void) { return 1; }
	EOF
	git add worddiff.c &&

	# newfile.c: single-line function, value changes 42 -> 99.
	cat >newfile.c <<-\EOF &&
	int new_func(void) { return 42; }
	EOF
	git add newfile.c &&

	# logtest.c: single-line function for log/format-patch tests.
	# Needs two commits so log -1 has a diff.
	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 1; }
	EOF
	git add logtest.c &&

	# one.c/two.c: two-file pair for error/abort/startup-failure tests.
	cat >one.c <<-\EOF &&
	int first(void) { return 1; }
	EOF
	cat >two.c <<-\EOF &&
	int second(void) { return 2; }
	EOF
	git add one.c two.c &&

	git commit -m "initial" &&

	# Second commit for logtest.c (so log -1 has something to show).
	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 2; }
	EOF
	git add logtest.c &&
	git commit -m "change logtest.c" &&

	# Working tree modifications (not committed).
	cat >boundary.c <<-\EOF &&
	line1
	line2
	line3
	line4
	NEW5
	NEW6
	line7
	line8
	NEW9
	NEW10
	EOF

	cat >worddiff.c <<-\EOF &&
	int value(void) { return 999; }
	EOF

	cat >newfile.c <<-\EOF &&
	int new_func(void) { return 99; }
	EOF

	cat >one.c <<-\EOF &&
	int first(void) { return 10; }
	EOF

	cat >two.c <<-\EOF
	int second(void) { return 20; }
	EOF
'

#
# Core behavior: the process controls which lines are marked as changed.
#

test_expect_success 'diff process hunk boundaries affect output' '
	# The file has changes at lines 5-6 and 9-10, but fixed-hunk
	# only reports lines 5-6 as changed.  Lines 9-10 should not
	# appear as changed in the output.
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk" \
		diff boundary.c >actual &&
	test_grep "^-OLD5" actual &&
	test_grep "^-OLD6" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^+NEW6" actual &&
	test_grep ! "^-OLD9" actual &&
	test_grep ! "^-OLD10" actual &&
	test_grep ! "^+NEW9" actual &&
	test_grep ! "^+NEW10" actual
'

test_expect_success 'diff process accepts valid multi-hunk output' '
	test_when_finished "rm -f backend.log" &&
	# mh.c changes lines 5-10, but multi-hunk reports only 5-6 and 9-10 as
	# two gap-aligned hunks, leaving 7-8 paired as context.  This exercises
	# the accepting branch of the per-gap lockstep check (non-zero
	# previous-hunk end) and discriminates honoring: the builtin diff would
	# show lines 7-8 as changed, so their absence proves the process hunks
	# were used, not a silent builtin fallback.
	cat >mh.c <<-\EOF &&
	line1
	line2
	line3
	line4
	OLD5
	OLD6
	OLD7
	OLD8
	OLD9
	OLD10
	EOF
	git add mh.c &&
	git commit -m "add mh.c" &&
	cat >mh.c <<-\EOF &&
	line1
	line2
	line3
	line4
	NEW5
	NEW6
	NEW7
	NEW8
	NEW9
	NEW10
	EOF
	git -c diff.cdiff.process="$BACKEND --mode=multi-hunk --log=backend.log" \
		diff mh.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual &&
	# 7-8 changed on disk but the process reported them unchanged, so they
	# appear as context, not as the -OLD7/-OLD8 the builtin would emit.
	test_grep ! "^-OLD7" actual &&
	test_grep ! "^-OLD8" actual &&
	test_must_be_empty stderr &&
	test_grep "command=hunks pathname=mh.c" backend.log
'

test_expect_success 'diff process accepts a mid-file count-0 insertion' '
	test_when_finished "rm -f backend.log" &&
	# insert mode reports "hunk 3 0 3 2": a pure insertion (count 0 on the
	# old side) of 2 lines before old line 3, in the protocol 1-based
	# position form.  The working change also rewrites the trailing lines,
	# but the process pairs old lines 3-5 with the new trailing lines as
	# unchanged, so the builtin diff (which would show -c/-d/-e and
	# +P/+Q/+R) diverges.  Exercises the count-0 insert path, which uses
	# the unshifted position, and discriminates honoring from a fallback.
	cat >insert.c <<-\EOF &&
	a
	b
	c
	d
	e
	EOF
	git add insert.c &&
	git commit -m "add insert.c" &&
	cat >insert.c <<-\EOF &&
	a
	b
	X
	Y
	P
	Q
	R
	EOF
	git -c diff.cdiff.process="$BACKEND --mode=insert --log=backend.log" \
		diff insert.c >actual 2>stderr &&
	test_grep "^+X" actual &&
	test_grep "^+Y" actual &&
	# The narrower insert pairs old lines 3-5 as context, so neither the
	# removed "c" nor the added "P" appears as a change; the builtin diff
	# would show both.
	test_grep ! "^-c" actual &&
	test_grep ! "^+P" actual &&
	test_grep "command=hunks pathname=insert.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process accepts a mid-file count-0 deletion' '
	test_when_finished "rm -f backend.log" &&
	# delete mode reports "hunk 3 2 3 0": a pure deletion (count 0 on the
	# new side) removing old lines 3-4.  The working change also rewrites
	# the last line, but the process pairs old line 5 with the new last
	# line as unchanged, so the builtin diff (which would show -e and +Z)
	# diverges.  Exercises the count-0 deletion path.
	cat >del.c <<-\EOF &&
	a
	b
	c
	d
	e
	EOF
	git add del.c &&
	git commit -m "add del.c" &&
	cat >del.c <<-\EOF &&
	a
	b
	Z
	EOF
	git -c diff.cdiff.process="$BACKEND --mode=delete --log=backend.log" \
		diff del.c >actual 2>stderr &&
	test_grep "^-c" actual &&
	test_grep "^-d" actual &&
	# The narrower deletion pairs old line 5 as context, so neither the
	# removed "e" nor the added "Z" appears as a change; the builtin diff
	# would show both.
	test_grep ! "^-e" actual &&
	test_grep ! "^+Z" actual &&
	test_grep "command=hunks pathname=del.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process works with modified file' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- newfile.c >actual 2>stderr &&
	test_grep "return 99" actual &&
	test_grep "pathname=newfile.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process works with added file (empty old side)' '
	cat >added.c <<-\EOF &&
	int added(void) { return 1; }
	EOF
	git add added.c &&

	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --cached -- added.c >actual 2>stderr &&
	test_grep "added" actual &&
	test_grep "pathname=added.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process works with deleted file (empty new side)' '
	git add added.c &&
	git commit -m "commit added.c" &&
	git rm added.c &&

	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --cached -- added.c >actual 2>stderr &&
	test_grep "deleted file" actual &&
	test_grep "pathname=added.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process skipped for binary files' '
	printf "\\0binary" >binary.c &&
	git add binary.c &&
	git commit -m "add binary" &&
	printf "\\0changed" >binary.c &&

	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- binary.c >actual &&
	test_grep "Binary files" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process not consulted for unmatched driver' '
	echo "not tracked by cdiff" >unmatched.txt &&
	git add unmatched.txt &&
	git commit -m "add unmatched.txt" &&

	echo "modified" >unmatched.txt &&

	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- unmatched.txt >actual &&
	test_grep "modified" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'multiple drivers use separate processes' '
	echo "*.h diff=hdiff" >>.gitattributes &&
	git add .gitattributes &&

	cat >multi.h <<-\EOF &&
	int header(void) { return 1; }
	EOF
	git add multi.h &&
	git commit -m "add multi.h" &&

	cat >multi.h <<-\EOF &&
	int header(void) { return 2; }
	EOF

	test_when_finished "rm -f backend-c.log backend-h.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend-c.log" \
	    -c diff.hdiff.process="$BACKEND --log=backend-h.log" \
		diff -- newfile.c multi.h >actual 2>stderr &&
	test_grep "pathname=newfile.c" backend-c.log &&
	test_grep "pathname=multi.h" backend-h.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process resolved by repo path from a subdirectory' '
	test_when_finished "rm -f backend.log" &&
	mkdir -p sub &&
	echo "sub/*.c diff=subdiff" >>.gitattributes &&
	printf "a\nb\nc\n" >sub/thing.c &&
	git add .gitattributes sub/thing.c &&
	git commit -m "add sub/thing.c" &&
	printf "a\nB\nc\n" >sub/thing.c &&
	# --relative from sub/ strips the display path to "thing.c".  The
	# driver must still resolve by the repo-relative path sub/thing.c
	# (which matches sub/*.c=subdiff); the stripped "thing.c" would match
	# *.c=cdiff, which has no process configured here.
	git -C sub -c diff.subdiff.process="$BACKEND --log=backend.log" \
		diff --relative thing.c >/dev/null &&
	test_grep "command=hunks pathname=sub/thing.c" backend.log
'

test_expect_success 'diff process works alongside textconv' '
	write_script uppercase-filter <<-\EOF &&
	tr "a-z" "A-Z" <"$1"
	EOF

	cat >textconv.c <<-\EOF &&
	hello world
	EOF
	git add textconv.c &&
	git commit -m "add textconv.c" &&

	cat >textconv.c <<-\EOF &&
	goodbye world
	EOF

	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.textconv="./uppercase-filter" \
	    -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- textconv.c >actual 2>stderr &&
	# The diff process receives textconv-transformed (uppercase) content.
	test_grep "pathname=textconv.c" backend.log &&
	test_grep "old=HELLO WORLD" backend.log &&
	test_grep "new=GOODBYE WORLD" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process --stat is fed raw, not textconv, content' '
	# Reuses textconv.c from the previous test (committed "hello
	# world", modified to "goodbye world").  Unlike patch output,
	# --stat does not apply textconv, so the process sees raw lowercase
	# content here even with a textconv configured.
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.textconv="./uppercase-filter" \
	    -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --stat -- textconv.c >actual 2>stderr &&
	test_grep "pathname=textconv.c" backend.log &&
	test_grep "old=hello world" backend.log &&
	test_grep "new=goodbye world" backend.log &&
	test_must_be_empty stderr
'

#
# Downstream features: word diff, log, equivalent files, exit code.
#

test_expect_success 'diff process with --word-diff' '
	test_when_finished "rm -f backend.log" &&
	# boundary.c changes lines 5-6 and 9-10, but fixed-hunk reports only
	# 5-6.  word-diff operates on the process hunks, so 5-6 render as word
	# changes while 9-10 stay context; the builtin would word-diff all four
	# lines, so the absent OLD9 marker proves the hunks were honored.
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		diff --word-diff boundary.c >actual 2>stderr &&
	test_grep "\[-OLD5-\]" actual &&
	test_grep "{+NEW5+}" actual &&
	test_grep ! "OLD9" actual &&
	test_grep "pathname=boundary.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process works with git log -p' '
	# With no-hunks mode, the process says the files are equivalent,
	# so log -p should show the commit but no diff content.
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		log -1 -p -- logtest.c >actual 2>stderr &&
	test_grep "change logtest.c" actual &&
	test_grep ! "return 2" actual &&
	test_grep "command=hunks pathname=logtest.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process no hunks suppresses diff output' '
	cat >nohunks.c <<-\EOF &&
	int zero(void) { return 0; }
	EOF
	git add nohunks.c &&
	git commit -m "add nohunks.c" &&

	cat >nohunks.c <<-\EOF &&
	int zero(void) { return 999; }
	EOF

	git -c diff.cdiff.process="$BACKEND --mode=no-hunks" \
		diff nohunks.c >actual &&
	test_must_be_empty actual
'

test_expect_success 'diff process no hunks with --exit-code returns success' '
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks" \
		diff --exit-code nohunks.c
'

test_expect_success 'diff process equivalent commit: --exit-code and --quiet agree' '
	# A committed blob pair (not a worktree file) whose oids differ but
	# the process reports equivalent.  --exit-code and --quiet must agree
	# with the shown diff (empty) and report success, not fall back to
	# the byte-level "oids differ" answer.
	cat >ecq.c <<-\EOF &&
	alpha
	EOF
	git add ecq.c &&
	git commit -m "ecq v1" &&
	cat >ecq.c <<-\EOF &&
	beta
	EOF
	git add ecq.c &&
	git commit -m "ecq v2" &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks" \
		diff --exit-code HEAD^ HEAD -- ecq.c &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks" \
		diff --quiet HEAD^ HEAD -- ecq.c
'

test_expect_success 'diff process falls back for trailing-newline-only change' '
	test_when_finished "rm -f backend.log" &&
	printf "a\nb\nc\n" >eofnl.c &&
	git add eofnl.c &&
	git commit -m "add eofnl.c" &&
	printf "a\nb\nc" >eofnl.c &&
	# Same lines, only the final newline removed.  The process reports
	# no hunks (it sees identical lines), but that change is not
	# expressible as hunks, so git falls back to the builtin diff
	# rather than treating the files as equivalent.
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff eofnl.c >actual 2>stderr &&
	test_grep "No newline at end of file" actual &&
	test_grep "pathname=eofnl.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process rejects no-change hunk for trailing-newline-only change' '
	test_when_finished "rm -f backend.log" &&
	printf "a\nb\nc\n" >noopnl.c &&
	git add noopnl.c &&
	git commit -m "add noopnl.c" &&
	printf "a\nb\nc" >noopnl.c &&
	# A 0/0 hunk names no changed lines.  Git must reject it and keep
	# the builtin trailing-newline marker instead of showing no diff.
	git -c diff.cdiff.process="$BACKEND --mode=noop-hunk --log=backend.log" \
		diff noopnl.c >actual 2>stderr &&
	test_grep "No newline at end of file" actual &&
	test_grep "hunk with no change" stderr &&
	test_grep "command=hunks pathname=noopnl.c" backend.log
'

test_expect_success 'diff process falls back for added file (empty old side)' '
	test_when_finished "rm -f backend.log" &&
	printf "x\ny\nz\n" >addnl.c &&
	git add addnl.c &&
	# The empty old side has no trailing newline while the new side
	# does, so the newline fallback shows the addition rather than
	# letting no-hunks suppress the whole new file.
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff --cached addnl.c >actual 2>stderr &&
	test_grep "^+x" actual &&
	test_grep "pathname=addnl.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process with --exit-code and hunks returns failure' '
	test_when_finished "rm -f backend.log" &&
	test_expect_code 1 git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		diff --exit-code boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep ! "^-OLD9" actual &&
	test_grep ! "^+NEW9" actual &&
	test_grep "command=hunks pathname=boundary.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process feeds --numstat counts' '
	# fixed-hunk reports only lines 5-6 as changed, so the stat
	# counts come from the process (2/2), not the builtin diff (4/4).
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		diff --numstat boundary.c >actual 2>stderr &&
	printf "2\t2\tboundary.c\n" >expect &&
	test_cmp expect actual &&
	test_grep "command=hunks pathname=boundary.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process --numstat sums multi-hunk counts' '
	test_when_finished "rm -f backend.log" &&
	# mhstat.c changes lines 5-10, but multi-hunk reports only 5-6 and
	# 9-10 (4 lines each way).  The builtin diff counts all six changed
	# lines (6/6), so the process counts (4/4) discriminate honoring and
	# exercise the two-region hunk path through builtin_diffstat.
	cat >mhstat.c <<-\EOF &&
	line1
	line2
	line3
	line4
	OLD5
	OLD6
	OLD7
	OLD8
	OLD9
	OLD10
	EOF
	git add mhstat.c &&
	git commit -m "add mhstat.c" &&
	cat >mhstat.c <<-\EOF &&
	line1
	line2
	line3
	line4
	NEW5
	NEW6
	NEW7
	NEW8
	NEW9
	NEW10
	EOF
	git -c diff.cdiff.process="$BACKEND --mode=multi-hunk --log=backend.log" \
		diff --numstat mhstat.c >actual &&
	printf "4\t4\tmhstat.c\n" >expect &&
	test_cmp expect actual &&
	# The builtin counts all six changed lines, so the two differ.
	git diff --no-ext-diff --numstat mhstat.c >builtin &&
	printf "6\t6\tmhstat.c\n" >expectbuiltin &&
	test_cmp expectbuiltin builtin &&
	test_grep "command=hunks pathname=mhstat.c" backend.log
'

test_expect_success 'diff process --stat resolved by repo path from a subdirectory' '
	test_when_finished "rm -f backend.log" &&
	# sub/thing.c and the sub/*.c=subdiff attribute exist from an earlier
	# test; builtin_diffstat must resolve the driver by the repo path too.
	printf "a\nB\nc\n" >sub/thing.c &&
	git -C sub -c diff.subdiff.process="$BACKEND --log=backend.log" \
		diff --relative --stat thing.c >/dev/null &&
	test_grep "command=hunks pathname=sub/thing.c" backend.log
'

test_expect_success 'diff process equivalent files produce no --stat line' '
	# A file the process calls equivalent contributes no stat line,
	# matching the empty patch that git diff produces for it.
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff --stat worddiff.c >actual 2>stderr &&
	test_must_be_empty actual &&
	test_grep "command=hunks pathname=worddiff.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process scopes --stat to the tracked range under log -L' '
	test_when_finished "rm -f backend.log" &&
	cat >rangestat.c <<-\EOF &&
	line1
	line2
	line3
	line4
	OLD5
	OLD6
	line7
	line8
	OLD9
	OLD10
	EOF
	git add rangestat.c &&
	git commit -m "add rangestat.c" &&

	cat >rangestat.c <<-\EOF &&
	line1
	line2
	line3
	line4
	NEW5
	NEW6
	line7
	line8
	NEW9
	NEW10
	EOF
	git add rangestat.c &&
	git commit -m "change rangestat.c" &&

	# The file changes at lines 5-6 and 9-10, but fixed-hunk reports
	# only 5-6.  The tracked range is 9-10, so the builtin stat counts
	# that region as 2 insertions and 2 deletions.  The process hunk 5-6
	# falls outside 9-10, so the line-range filter drops it and the change
	# commit shows no counts.  The add commit still adds lines 9-10, so a
	# blanket "no insertions" check would match it; assert instead that the
	# change commit loses its two-sided count.
	git log --no-ext-diff -L9,10:rangestat.c --oneline --stat >builtin &&
	test_grep "2 insertions(+), 2 deletions(-)" builtin &&

	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		log -L9,10:rangestat.c --oneline --stat >actual &&
	test_grep "change rangestat.c" actual &&
	test_grep ! "2 insertions(+), 2 deletions(-)" actual &&
	test_grep "command=hunks pathname=rangestat.c" backend.log
'

test_expect_success 'diff process equivalent file makes --stat --exit-code succeed' '
	# The process reports worddiff.c equivalent, so --exit-code reports
	# no change (0); the builtin diff would report a change (1).
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks" \
		diff --stat --exit-code worddiff.c &&
	test_expect_code 1 git diff --no-ext-diff --stat --exit-code worddiff.c
'

test_expect_success 'diff process --numstat with mixed equivalent and changed files' '
	test_when_finished "rm -f c.log h.log" &&
	# Self-contained fixtures: *.c uses whole-file (changed); *.mh
	# uses no-hunks (equivalent).
	echo "*.mh diff=hdiff" >>.gitattributes &&
	git add .gitattributes &&
	printf "int a(void) { return 1; }\n" >mixed.c &&
	printf "int b(void) { return 1; }\n" >mixed.mh &&
	git add mixed.c mixed.mh &&
	git commit -m "add mixed fixtures" &&
	printf "int a(void) { return 2; }\n" >mixed.c &&
	printf "int b(void) { return 2; }\n" >mixed.mh &&
	git -c diff.cdiff.process="$BACKEND --mode=whole-file --log=c.log" \
	    -c diff.hdiff.process="$BACKEND --mode=no-hunks --log=h.log" \
		diff --numstat mixed.c mixed.mh >actual 2>stderr &&
	test_grep "mixed.c" actual &&
	test_grep ! "mixed.mh" actual &&
	test_grep "pathname=mixed.c" c.log &&
	test_grep "pathname=mixed.mh" h.log &&
	test_must_be_empty stderr
'

test_expect_success POSIXPERM 'diff process keeps mode-only change in --stat' '
	test_when_finished "rm -f backend.log" &&
	cat >modeonly.c <<-\EOF &&
	int m(void) { return 1; }
	EOF
	git add modeonly.c &&
	git commit -m "add modeonly.c" &&
	cat >modeonly.c <<-\EOF &&
	int m(void) { return 2; }
	EOF
	git add modeonly.c &&
	test_chmod +x modeonly.c &&
	git commit -m "edit and chmod modeonly.c" &&
	# Content and mode both changed, but no-hunks reports the content
	# equivalent.  The process is consulted (counts are zero, not the
	# builtin 1/1), yet the mode change keeps the file from being
	# pruned.
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff --stat HEAD^ HEAD >actual 2>stderr &&
	test_grep "modeonly.c" actual &&
	test_grep "command=hunks pathname=modeonly.c" backend.log &&
	test_grep ! "1 insertion" actual &&
	test_must_be_empty stderr
'

test_expect_success 'diff process not consulted for default --dirstat' '
	# The default (change-based) --dirstat algorithm counts via its
	# own path and never contacts the process (here --dirstat=0 just
	# sets a 0% threshold), so the change is still reported even
	# though no-hunks would call it equivalent.  --dirstat=lines
	# instead uses the process-aware stat path.
	test_when_finished "rm -f backend.log" &&
	mkdir -p dsub &&
	printf "a\nb\nc\n" >dsub/d.c &&
	git add dsub/d.c &&
	git commit -m "add dsub/d.c" &&
	printf "a\nB\nc\n" >dsub/d.c &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff --dirstat=0 dsub/d.c >actual &&
	test_grep "dsub" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process consulted for --dirstat=lines' '
	test_when_finished "rm -f backend.log" &&
	# --dirstat=lines uses the process-aware stat path (unlike the default
	# byte-weighted --dirstat above).  no-hunks reports dsub/d.c
	# equivalent, so it contributes no changed lines and dsub is not
	# listed; the builtin diff would count the change and list it.
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		diff --dirstat=lines dsub/d.c >actual &&
	test_must_be_empty actual &&
	test_grep "command=hunks pathname=dsub/d.c" backend.log &&
	git diff --no-ext-diff --dirstat=lines dsub/d.c >builtin &&
	test_grep "dsub" builtin
'

#
# Bypass mechanisms: flags and commands that skip the diff process.
#

test_expect_success 'diff process bypassed by --diff-algorithm' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --diff-algorithm=patience worddiff.c >actual &&
	test_grep "return 999" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process honors hunks under diff.algorithm=histogram' '
	test_when_finished "rm -f backend.log" &&
	# diff.algorithm config (unlike the --diff-algorithm option) does
	# not bypass the process, so the process is consulted and its hunks
	# must stand: the histogram algorithm must not re-diff and reclaim
	# lines the process left unchanged.  fixed-hunk marks only lines 5-6,
	# so 9-10 stay unchanged even though their content differs.
	git -c diff.algorithm=histogram \
	    -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		diff boundary.c >actual &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep ! "^-OLD9" actual &&
	test_grep ! "^+NEW9" actual &&
	test_grep "pathname=boundary.c" backend.log
'

test_expect_success 'diff process bypassed by --no-ext-diff' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --no-ext-diff worddiff.c >actual &&
	test_grep "return 999" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process not used by format-patch' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		format-patch -1 --stdout -- logtest.c >actual &&
	test_grep "return 2" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'diff process bypassed under whitespace-ignoring flags' '
	test_when_finished "rm -f backend.log" &&
	printf "a\nb\nc\n" >wsbypass.c &&
	git add wsbypass.c &&
	git commit -m "add wsbypass.c" &&
	printf "a\n  b  \nc\n" >wsbypass.c &&
	# The process is never told about these options and could not honor
	# them, so git bypasses the process for each (covering the whole
	# XDF_WHITESPACE_FLAGS | XDF_IGNORE_BLANK_LINES mask, not just -w).
	for opt in -w -b --ignore-space-at-eol --ignore-blank-lines
	do
		rm -f backend.log &&
		git -c diff.cdiff.process="$BACKEND --log=backend.log" \
			diff $opt wsbypass.c >actual 2>stderr &&
		test_path_is_missing backend.log &&
		test_must_be_empty stderr ||
		return 1
	done &&
	# -w additionally suppresses the whitespace-only change via the
	# builtin diff that now runs.
	git -c diff.cdiff.process="$BACKEND" diff -w wsbypass.c >actual &&
	test_must_be_empty actual
'

#
# Error handling and fallback.
#

test_expect_success 'diff process fallback on an error status' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=error --log=backend.log" \
		diff boundary.c >actual 2>stderr &&
	# Fallback produces the full builtin diff (both change regions).
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual &&
	# The process was contacted (it replied with error, not crash).
	test_grep "command=hunks pathname=boundary.c" backend.log &&
	test_grep "diff process.*failed" stderr
'

test_expect_success 'diff process error keeps the process available for next file' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=error --log=backend.log" \
		diff -- one.c two.c >actual 2>stderr &&
	# Unlike abort, error keeps the process available: both files
	# are sent to the process (and both fall back).
	test_grep "pathname=one.c" backend.log &&
	test_grep "pathname=two.c" backend.log &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	test_grep "diff process.*failed" stderr
'

test_expect_success 'diff process abort disables for session' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=abort --log=backend.log" \
		diff -- one.c two.c >actual 2>stderr &&
	# Both files should still produce diff output via fallback.
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	# The process aborts on the first file (one.c, which sorts before
	# two.c and is therefore diffed first) and git clears its capability.
	# The second file never contacts the process.
	test_grep "pathname=one.c" backend.log &&
	test_grep ! "pathname=two.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process empty hunk packet falls back instead of hiding a change' '
	test_when_finished "rm -f backend.log requests warnings" &&
	git -c diff.cdiff.process="$BACKEND --mode=content-empty-packet --log=backend.log" \
		diff -- one.c two.c >actual 2>stderr &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	test_grep "pathname=one.c" backend.log &&
	test_grep ! "pathname=two.c" backend.log &&
	grep "command=hunks" backend.log >requests &&
	test_line_count = 1 requests &&
	grep "diff process.*failed" stderr >warnings &&
	test_line_count = 1 warnings
'

test_expect_success 'diff process empty hunk packet before a status still shows the change' '
	test_when_finished "rm -f backend.log" &&
	# The process sends an empty packet, then status=success with no flush
	# between them.  Reading the empty packet as a flush would consume the
	# status as no hunks and hide the change.  Git must reject the empty
	# packet and fall back to the builtin diff.
	git -c diff.cdiff.process="$BACKEND --mode=content-empty-then-status --log=backend.log" \
		diff -- one.c two.c >actual 2>stderr &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	test_grep "pathname=one.c" backend.log
'

test_expect_success 'diff process missing status drops the process for the session' '
	test_when_finished "rm -f backend.log requests warnings" &&
	git -c diff.cdiff.process="$BACKEND --mode=content-missing-status --log=backend.log" \
		diff -- one.c two.c >actual 2>stderr &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	test_grep "pathname=one.c" backend.log &&
	test_grep ! "pathname=two.c" backend.log &&
	grep "command=hunks" backend.log >requests &&
	test_line_count = 1 requests &&
	grep "diff process.*failed" stderr >warnings &&
	test_line_count = 1 warnings
'

test_expect_success 'diff process fallback on a process crash' '
	git -c diff.cdiff.process="$BACKEND --mode=crash" \
		diff boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual &&
	# Crash is a communication failure, so a warning is emitted.
	test_grep "diff process.*failed" stderr
'

test_expect_success 'diff process startup failure only warns once' '
	git -c diff.cdiff.process="/nonexistent/diff-process" \
		diff -- one.c two.c >actual 2>stderr &&
	# Both files produce diff output via fallback.
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	# Sentinel prevents repeated warnings: only one, not one per file.
	test_grep "diff process.*failed" stderr >warnings &&
	test_line_count = 1 warnings
'


test_expect_success 'diff process fallback on bad hunks' '
	git -c diff.cdiff.process="$BACKEND --mode=bad-hunk" \
		diff boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual &&
	test_grep "hunk past the end" stderr
'

test_expect_success 'diff process fallback on mismatched unchanged totals' '
	cat >synctest.c <<-\EOF &&
	line1
	line2
	line3
	EOF
	git add synctest.c &&
	git commit -m "add synctest.c" &&

	cat >synctest.c <<-\EOF &&
	line1
	changed
	line3
	EOF

	# bad-sync reports hunk 1 2 1 1: marks 2 old lines and 1 new
	# line as changed, leaving 1 unchanged old vs 2 unchanged new.
	# The synchronization invariant fails and git falls back.
	git -c diff.cdiff.process="$BACKEND --mode=bad-sync" \
		diff synctest.c >actual 2>stderr &&
	test_grep "changed" actual &&
	test_grep "misaligned" stderr
'

test_expect_success 'diff process fallback on misaligned hunk gap' '
	# bad-gap reports hunk 1 1 3 1 on boundary.c: one changed line
	# on each side, so the total unchanged counts match, but the
	# unchanged run before the change differs (old line 1 vs new
	# line 3).  A global count check would accept this and emit a
	# corrupt diff; the per-gap lockstep check rejects it and git
	# falls back to the builtin algorithm.
	git -c diff.cdiff.process="$BACKEND --mode=bad-gap" \
		diff boundary.c >actual 2>stderr &&
	# The builtin fallback shows both changed regions as additions
	# (a corrupt-accepted hunk would show NEW5 only as context).
	test_grep "^+NEW5" actual &&
	test_grep "^+NEW9" actual &&
	test_grep "misaligned" stderr
'

test_expect_success 'diff process fallback on overlapping hunks' '
	# boundary.c has 10 lines, so both hunks are in bounds, but the
	# second hunk starts before the end of the first, triggering the
	# ordering check.
	git -c diff.cdiff.process="$BACKEND --mode=overlap" \
		diff boundary.c >actual 2>stderr &&
	test_grep "NEW5" actual &&
	test_grep "overlapping hunks" stderr
'

test_expect_success 'diff process fallback on malformed hunk line' '
	git -c diff.cdiff.process="$BACKEND --mode=bad-parse" \
		diff boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	# A parse failure is a communication error, not "no hunks", so it
	# must warn and fall back rather than treat the files as equal.
	test_grep "diff process.*failed" stderr
'

test_expect_success 'diff process fallback on start 0 with nonzero count' '
	# bad-start reports hunk 0 1 1 1.  A start of 0 is valid only for
	# an empty (count 0) range, so the presentation-to-xdiff
	# translation rejects it and git falls back to the builtin diff
	# instead of handing xdiff an out-of-range start.
	git -c diff.cdiff.process="$BACKEND --mode=bad-start" \
		diff boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "diff process.*failed" stderr
'

test_expect_success 'diff process caps a flood of hunks and falls back' '
	# flood emits far more hunks than the file has lines.  Git must
	# stop accumulating and fall back to the builtin diff rather than
	# grow memory without bound.
	git -c diff.cdiff.process="$BACKEND --mode=flood" \
		diff boundary.c >actual 2>stderr &&
	test_grep "^-OLD5" actual &&
	test_grep "too many hunks" stderr
'

test_expect_success 'diff process skipped when the process omits capability' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=no-cap --log=backend.log" \
		diff boundary.c >actual 2>stderr &&
	# Builtin diff runs: all changes appear, including lines 9-10
	# that a process-provided hunk would have narrowed away.
	test_grep "^-OLD5" actual &&
	test_grep "^-OLD9" actual &&
	# The process launched (creating the log) but was
	# never sent a per-file request, so no hunks command is logged.
	test_path_is_file backend.log &&
	test_grep ! "command=hunks" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process receives old-oid and new-oid for a blob pair' '
	test_when_finished "rm -f backend.log" &&
	cat >oidpair.c <<-\EOF &&
	int f(void) { return 1; }
	EOF
	git add oidpair.c &&
	git commit -m "oidpair v1" &&
	old=$(git rev-parse HEAD:oidpair.c) &&

	cat >oidpair.c <<-\EOF &&
	int f(void) { return 2; }
	EOF
	git add oidpair.c &&
	git commit -m "oidpair v2" &&
	new=$(git rev-parse HEAD:oidpair.c) &&

	# Both sides are stored blobs, so their object ids are sent.
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff HEAD^ HEAD -- oidpair.c >actual 2>stderr &&
	test_grep "old-oid=$old new-oid=$new" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'diff process omits old-oid and new-oid for textconv content' '
	test_when_finished "rm -f backend.log" &&
	write_script oidcat <<-\EOF &&
	cat "$1"
	EOF
	cat >oidtc.c <<-\EOF &&
	alpha
	EOF
	git add oidtc.c &&
	git commit -m "oidtc v1" &&
	cat >oidtc.c <<-\EOF &&
	beta
	EOF
	git add oidtc.c &&
	git commit -m "oidtc v2" &&

	# textconv rewrites the bytes, so the raw-blob object id that
	# would otherwise identify each side is omitted.
	git -c diff.cdiff.textconv="./oidcat" \
	    -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff HEAD^ HEAD -- oidtc.c >actual 2>stderr &&
	test_grep "pathname=oidtc.c" backend.log &&
	test_grep "old-oid=(none) new-oid=(none)" backend.log &&
	test_must_be_empty stderr
'

#
# Blame integration.
#

test_expect_success 'blame uses process-provided hunks' '
	cat >blame-hunk.c <<-\EOF &&
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
	git add blame-hunk.c &&
	git commit -m "add blame-hunk.c" &&
	ORIG=$(git rev-parse --short HEAD) &&

	cat >blame-hunk.c <<-\EOF &&
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
	git add blame-hunk.c &&
	git commit -m "change blame-hunk.c" &&
	CHANGE=$(git rev-parse --short HEAD) &&

	# The process answers by object id (no blob loaded) reporting only
	# lines 5-6 as changed, so blame should attribute lines 9-10 to the
	# original commit even though the builtin diff would show them as
	# changed.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed" \
		blame blame-hunk.c >actual &&
	sed -n "9p" actual >line9 &&
	sed -n "10p" actual >line10 &&
	test_grep "$ORIG" line9 &&
	test_grep "$ORIG" line10 &&
	sed -n "5p" actual >line5 &&
	sed -n "6p" actual >line6 &&
	test_grep "$CHANGE" line5 &&
	test_grep "$CHANGE" line6
'

test_expect_success 'blame skips commits with no hunks from diff process' '
	cat >blame.c <<-\EOF &&
	int main(void) {
	return 0;
	}
	EOF
	git add blame.c &&
	git commit -m "add blame.c" &&
	ORIG_COMMIT=$(git rev-parse --short HEAD) &&

	cat >blame.c <<-\EOF &&
	int main(void)
	{
	return 1;
	}
	EOF
	git add blame.c &&
	git commit -m "change blame.c" &&
	BLAME_COMMIT=$(git rev-parse --short HEAD) &&

	# Without no-hunks mode, blame attributes the change.
	git blame blame.c >without &&
	test_grep "$BLAME_COMMIT" without &&

	# oid-no-hunks reports equal line counts and no hunks.  The process
	# reports the files equivalent, so blame skips the change commit.
	git -c diff.cdiff.process="$BACKEND --mode=oid-no-hunks" \
		blame blame.c >with &&
	test_grep ! "$BLAME_COMMIT" with &&
	test_grep "$ORIG_COMMIT" with
'

test_expect_success 'blame falls back on a by-oid no-hunks reply with unequal line counts' '
	test_when_finished "rm -f backend.log" &&
	printf "alpha\nbeta\ngamma\n" >oid_unequal.c &&
	git add oid_unequal.c &&
	git commit -m "add oid_unequal.c" &&
	OID_UNEQUAL_ORIG=$(git rev-parse HEAD) &&
	printf "alpha\nbeta\nGAMMA\ndelta\n" >oid_unequal.c &&
	git add oid_unequal.c &&
	git commit -m "change oid_unequal.c" &&

	git blame --line-porcelain oid_unequal.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-no-hunks-different-lines --log=backend.log" \
		blame --line-porcelain oid_unequal.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep ! -E "^\^?$OID_UNEQUAL_ORIG 4 " actual &&
	test_grep "command=hunks-by-oid pathname=oid_unequal.c" backend.log &&
	test_grep ! "command=hunks pathname" backend.log &&
	test_must_be_empty stderr
'

test_expect_success 'blame reads no parent blob when answered by object id' '
	test_when_finished "rm -f b.log" &&
	# by_oid.c is 10 lines; lines 5-6 change across three commits.  With
	# the process answering by object id, blame reads only the final blob
	# it displays, not the parent blob of each pair it diffs.
	printf "a\nb\nc\nd\nA5\nA6\ng\nh\ni\nj\n" >by_oid.c &&
	git add by_oid.c && git commit -q -m "by_oid v1" &&
	printf "a\nb\nc\nd\nB5\nB6\ng\nh\ni\nj\n" >by_oid.c &&
	git commit -q -am "by_oid v2" &&
	printf "a\nb\nc\nd\nC5\nC6\ng\nh\ni\nj\n" >by_oid.c &&
	git commit -q -am "by_oid v3" &&

	git blame --show-stats by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=b.log" \
		blame --show-stats by_oid.c >by_oid_out &&
	# Same attribution (drop the trailing "num ..." stat lines).
	grep -v "^num " builtin_out >builtin_blame &&
	grep -v "^num " by_oid_out >by_oid_blame &&
	test_cmp builtin_blame by_oid_blame &&
	# ...but fewer blob reads, and the process was asked by object id
	# only, never in content mode.  Content mode would load the blobs.
	test "$(sed -n "s/num read blob: //p" by_oid_out)" -lt \
	     "$(sed -n "s/num read blob: //p" builtin_out)" &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" b.log &&
	test_grep ! "command=hunks pathname" b.log
'

test_expect_success 'blame falls back on an out-of-bounds by-oid hunk' '
	test_when_finished "rm -f backend.log" &&
	# oid-bad-hunk answers by object id with a hunk past the end of the
	# blob.  Git validates it against the process-reported line counts,
	# rejects it, warns, and uses the builtin diff.
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-bad-hunk --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "past the end" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame falls back on a by-oid error status' '
	test_when_finished "rm -f backend.log requests warnings" &&
	# oid-error answers by object id with a well-framed reply that ends in
	# status=error.  blame warns and uses the builtin diff.
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-error --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "failed answering by object id" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	grep "command=hunks-by-oid" backend.log >requests &&
	test_line_count = 1 requests &&
	grep "failed answering by object id" stderr >warnings &&
	test_line_count = 1 warnings &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame falls back on a by-oid abort status' '
	test_when_finished "rm -f backend.log requests warnings" &&
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-abort --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "failed answering by object id" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	grep "command=hunks-by-oid" backend.log >requests &&
	test_line_count = 1 requests &&
	grep "failed answering by object id" stderr >warnings &&
	test_line_count = 1 warnings &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame falls back on a by-oid process crash' '
	test_when_finished "rm -f backend.log requests warnings" &&
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-crash --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "failed answering by object id" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	grep "command=hunks-by-oid" backend.log >requests &&
	test_line_count = 1 requests &&
	grep "failed answering by object id" stderr >warnings &&
	test_line_count = 1 warnings &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame falls back on a by-oid line count past the limit' '
	test_when_finished "rm -f backend.log" &&
	# oid-huge-lines answers by object id with a line count past
	# MAX_XDIFF_SIZE.  Git rejects the header before reading any hunk,
	# warns, and uses the builtin diff.
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-huge-lines --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "failed answering by object id" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame falls back on a missing by-oid lines header' '
	test_when_finished "rm -f backend.log" &&
	# oid-bad-lines answers by object id with a hunk line where the
	# mandatory "lines <old> <new>" header belongs.  Git cannot validate
	# the response, warns, and uses the builtin diff.
	git blame by_oid.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-bad-lines --log=backend.log" \
		blame by_oid.c >actual 2>stderr &&
	test_cmp builtin_out actual &&
	test_grep "failed answering by object id" stderr &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	test_grep ! "command=hunks pathname" backend.log
'

test_expect_success 'blame withholds by-oid for uncommitted working-tree content' '
	test_when_finished "rm -f backend.log" &&
	printf "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n" >wtblame.c &&
	git add wtblame.c && git commit -q -m "add wtblame.c" &&
	printf "a\nb\nC\nd\ne\nf\ng\nh\ni\nj\n" >wtblame.c &&
	# The working-tree blob is a pretend object, not written to the object
	# store, so a diff process cannot read it by object id.  blame
	# withholds the by-oid consult for the working-tree pass and uses the
	# builtin diff: no request is sent (the process is never launched) and
	# there is no spurious warning.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame wtblame.c >actual 2>stderr &&
	test_grep "Not Committed Yet" actual &&
	test_must_be_empty stderr &&
	test_path_is_missing backend.log
'

test_expect_success 'process advertising only hunks-by-oid: blame uses it, diff skips it' '
	test_when_finished "rm -f backend.log diff.log" &&
	# oid-only advertises hunks-by-oid but not hunks.  blame consults it by
	# object id; git diff (a content consumer) cannot use it and runs the
	# builtin without a per-file request.
	git -c diff.cdiff.process="$BACKEND --mode=oid-only --log=backend.log" \
		blame by_oid.c >actual &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log &&
	test_when_finished "git checkout -- by_oid.c" &&
	printf "a\nb\nc\nd\nZ5\nC6\ng\nh\ni\nj\n" >by_oid.c &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-only --log=diff.log" \
		diff by_oid.c >dactual &&
	test_grep "^-C5" dactual &&
	test_grep ! "command=" diff.log
'

test_expect_success 'blame --no-ext-diff bypasses diff process' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=no-hunks --log=backend.log" \
		blame --no-ext-diff blame.c >actual &&
	# Without the process, blame attributes the change commit normally.
	test_grep "$BLAME_COMMIT" actual &&
	test_path_is_missing backend.log
'

test_expect_success 'blame --no-ext-diff uses builtin hunks' '
	# fixed-hunk mode would narrow blame to lines 5-6, but
	# --no-ext-diff should bypass it and use the builtin diff.
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		blame --no-ext-diff blame-hunk.c >actual &&
	# Builtin diff attributes lines 9-10 to the change commit.
	sed -n "9p" actual >line9 &&
	test_grep "$CHANGE" line9 &&
	test_path_is_missing backend.log
'

test_expect_success 'blame -w bypasses diff process' '
	test_when_finished "rm -f backend.log" &&
	printf "alpha\nbeta\ngamma\n" >blamew.c &&
	git add blamew.c &&
	git commit -m "add blamew.c" &&
	orig=$(git rev-parse --short HEAD) &&
	printf "alpha\n   beta   \ngamma\n" >blamew.c &&
	git commit -am "reindent beta" &&
	reindent=$(git rev-parse --short HEAD) &&
	# blame -w must ignore the whitespace-only change and attribute
	# beta to the original commit, not the reindent commit.  The process
	# is never told about -w, so blame must bypass it (not let process
	# hunks override -w).
	git -c diff.cdiff.process="$BACKEND --mode=whole-file --log=backend.log" \
		blame -w blamew.c >actual &&
	sed -n "2p" actual >line2 &&
	test_grep "$orig" line2 &&
	test_grep ! "$reindent" line2 &&
	test_path_is_missing backend.log
'

test_expect_success 'blame --diff-algorithm bypasses the process' '
	test_when_finished "rm -f backend.log" &&
	# An explicit --diff-algorithm overrides the diff process, as it does
	# for git diff.  The process is not consulted.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame --diff-algorithm=histogram by_oid.c >actual &&
	test_path_is_missing backend.log
'

test_expect_success 'blame --histogram bypasses the process' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame --histogram blame-hunk.c >actual &&
	sed -n "9p" actual >line9 &&
	test_grep "$CHANGE" line9 &&
	test_path_is_missing backend.log
'

test_expect_success 'blame --patience bypasses the process' '
	test_when_finished "rm -f backend.log" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame --patience blame-hunk.c >actual &&
	sed -n "9p" actual >line9 &&
	test_grep "$CHANGE" line9 &&
	test_path_is_missing backend.log
'

test_expect_success 'blame consults the process under diff.algorithm config' '
	test_when_finished "rm -f backend.log" &&
	# diff.algorithm config (unlike the --diff-algorithm option) must not
	# bypass the process, matching git diff.
	git -c diff.algorithm=histogram \
	    -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		blame by_oid.c >actual &&
	test_grep "command=hunks-by-oid pathname=by_oid.c" backend.log
'

test_expect_success 'blame withholds by-oid when renamed target has textconv' '
	test_when_finished "rm -f backend.log tc-filter" &&
	write_script tc-filter <<-\EOF &&
	cat "$1"
	EOF
	echo "*.tc diff=tcdiff" >>.gitattributes &&
	git add .gitattributes &&
	cat >rename-old.c <<-\EOF &&
	a
	b
	c
	d
	e5
	f6
	g
	h
	i
	j
	EOF
	git add rename-old.c &&
	git commit -q -m "add rename-old.c" &&
	orig=$(git rev-parse --short HEAD) &&
	git mv rename-old.c rename-new.tc &&
	cat >rename-new.tc <<-\EOF &&
	a
	b
	c
	d
	E5
	F6
	g
	h
	i
	j
	EOF
	git add rename-new.tc &&
	git commit -q -m "rename to textconv path" &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
	    -c diff.tcdiff.textconv="./tc-filter" \
		blame rename-new.tc >actual 2>stderr &&
	sed -n "1p" actual >line1 &&
	test_grep "$orig" line1 &&
	test_must_be_empty stderr &&
	test_path_is_missing backend.log
'

test_expect_success 'blame falls back when the process lacks hunks-by-oid' '
	test_when_finished "rm -f backend.log" &&
	printf "1\n2\n3\n4\n5\n" >skip.c &&
	git add skip.c && git commit -q -m "skip v1" &&
	printf "1\n2\nX\n4\n5\n" >skip.c &&
	git commit -q -am "skip v2" &&
	# fixed-hunk advertises hunks (content) but not hunks-by-oid.  blame
	# cannot ask by object id, so it uses the builtin diff.
	git blame skip.c >builtin_out &&
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk --log=backend.log" \
		blame skip.c >actual &&
	test_cmp builtin_out actual &&
	# blame sent no per-file request: not by object id, not content.
	test_grep ! "command=" backend.log
'

#
# Line-log (git log -L) range tracking.
#

test_expect_success 'diff process drops equivalent commit from log -L' '
	test_when_finished "rm -f backend.log" &&
	cat >linelog.c <<-\EOF &&
	int tracked(void) { return 1; }
	EOF
	git add linelog.c &&
	git commit -m "add linelog.c" &&

	cat >linelog.c <<-\EOF &&
	int tracked(void) { return 2; }
	EOF
	git commit -am "change tracked line" &&

	# Builtin line tracking selects the change commit.
	git log --no-ext-diff -L1,1:linelog.c --format="%s" >builtin &&
	test_grep "change tracked line" builtin &&

	# Answering by object id with no hunks, the process reports the change
	# as equivalent, so tracking drops the commit (the range maps across
	# unchanged) instead of selecting it and rendering an empty diff.
	git -c diff.cdiff.process="$BACKEND --mode=oid-no-hunks --log=backend.log" \
		log -L1,1:linelog.c --format="%s" >actual &&
	test_grep ! "change tracked line" actual &&
	# The creating commit still appears, so the change commit was
	# selectively dropped rather than the whole log going empty.
	test_grep "add linelog.c" actual &&
	test_grep "command=hunks-by-oid pathname=linelog.c" backend.log
'

test_expect_success 'log -L keeps a commit when a by-oid no-hunks reply has unequal line counts' '
	test_when_finished "rm -f backend.log" &&
	git log --no-ext-diff -L3,3:oid_unequal.c --format="%s" >builtin &&
	test_grep "change oid_unequal.c" builtin &&

	git -c diff.cdiff.process="$BACKEND --mode=oid-no-hunks-different-lines --log=backend.log" \
		log -L3,3:oid_unequal.c --format="%s" >actual &&
	test_grep "change oid_unequal.c" actual &&
	test_grep "command=hunks-by-oid pathname=oid_unequal.c" backend.log
'

test_expect_success 'diff process tracks a by-oid hunk under log -L' '
	test_when_finished "rm -f backend.log" &&
	printf "a\nb\nc\nd\ne5\nf6\ng\nh\ni\nj\n" >linelog2.c &&
	git add linelog2.c && git commit -q -m "add linelog2.c" &&
	printf "a\nb\nc\nd\nE5\nF6\ng\nh\ni\nj\n" >linelog2.c &&
	git commit -q -am "change lines 5-6" &&

	# Builtin tracking of lines 5-6 selects the change commit.
	git log --no-ext-diff -L5,6:linelog2.c --format="%s" >builtin &&

	# The process answers by object id that lines 5-6 changed, matching the
	# real change, so by-oid tracking selects the same commits.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -L5,6:linelog2.c --format="%s" >actual &&
	test_cmp builtin actual &&
	test_grep "command=hunks-by-oid pathname=linelog2.c" backend.log
'

test_expect_success 'diff process narrows log -L selection by object id' '
	test_when_finished "rm -f backend.log" &&
	# linelog3.c really changes lines 5-6 and 9-10, but oid-fixed reports
	# only 5-6.  Tracking lines 9-10 by object id therefore sees no change
	# and drops the commit, while the builtin diff would keep it.  Tracking
	# the whole range 1-10 still keeps it, since 5-6 did change.  If the
	# by-oid answer were silently ignored, the narrow range would keep the
	# commit too and this test would fail.
	printf "a\nb\nc\nd\ne5\nf6\ng\nh\ni9\nj10\n" >linelog3.c &&
	git add linelog3.c && git commit -q -m "add linelog3.c" &&
	printf "a\nb\nc\nd\nE5\nF6\ng\nh\nI9\nJ10\n" >linelog3.c &&
	git commit -q -am "change lines 5-6 and 9-10" &&

	# Builtin tracking of lines 9-10 selects the change commit.
	git log --no-ext-diff -L9,10:linelog3.c --format="%s" >builtin &&
	test_grep "change lines 5-6 and 9-10" builtin &&

	# By object id the process reports only 5-6, so tracking 9-10 drops it.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -L9,10:linelog3.c --format="%s" >narrow &&
	test_grep ! "change lines 5-6 and 9-10" narrow &&
	# Tracking the whole range keeps it.
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed" \
		log -L1,10:linelog3.c --format="%s" >wide &&
	test_grep "change lines 5-6 and 9-10" wide &&
	test_grep "command=hunks-by-oid pathname=linelog3.c" backend.log
'

test_expect_success 'log -w -L withholds the by-oid consult' '
	test_when_finished "rm -f backend.log" &&
	# Builtin tracking keeps the real 9-10 change.  oid-fixed would report
	# only 5-6, so a by-oid request would drop this commit.
	git log --no-ext-diff -w -L9,10:linelog3.c --format="%s" >builtin &&
	test_grep "change lines 5-6 and 9-10" builtin &&
	: >backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -w -L9,10:linelog3.c --format="%s" >actual &&
	test_cmp builtin actual &&
	test_grep ! "command=hunks-by-oid" backend.log
'

test_expect_success 'log -L with textconv withholds the by-oid consult' '
	test_when_finished "rm -f backend.log" &&
	write_script lc-filter <<-\EOF &&
	tr "A-Z" "a-z" <"$1"
	EOF
	printf "a\nb\nc\nd\nE5\nF6\ng\nh\ni\nj\n" >lctc.c &&
	git add lctc.c && git commit -q -m "add lctc.c" &&
	printf "a\nb\nc\nd\nX5\nY6\ng\nh\ni\nj\n" >lctc.c &&
	git commit -q -am "change lctc.c" &&
	# oid-fixed advertises hunks-by-oid, so without a textconv gate git log
	# -L would consult by object id.  With textconv configured, the -L body
	# renders from transformed content, so the by-oid tracking is withheld
	# to stay consistent with the display; tracking uses the builtin diff,
	# and the process is consulted only for the body (content mode).
	git -c diff.cdiff.textconv="./lc-filter" \
	    -c diff.cdiff.process="$BACKEND --mode=oid-fixed --log=backend.log" \
		log -L5,6:lctc.c --format="%s" >actual &&
	test_grep "change lctc.c" actual &&
	test_grep ! "command=hunks-by-oid" backend.log &&
	test_grep "command=hunks pathname=lctc.c" backend.log
'

test_done
