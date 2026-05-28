#!/bin/sh

test_description='diff process via long-running process'

. ./test-lib.sh

if test_have_prereq PYTHON
then
	PYTHON_PATH=$(command -v python3) || PYTHON_PATH=$(command -v python)
fi

#
# A single parametric diff process.
# Usage: diff-process-backend --mode=<mode> [--log=<path>]
#
# Modes:
#   whole-file  - report all lines as changed (default)
#   fixed-hunk  - always report hunk 5 2 5 2
#   bad-hunk    - report out-of-bounds hunk 999 1 999 1
#   bad-sync    - report hunk with mismatched unchanged totals
#   overlap     - report two overlapping hunks
#   zero-hunk   - return no hunks (files considered equivalent)
#   error       - return status=error for every request
#   abort       - return status=abort for every request
#   crash       - read one request then exit without responding
#
setup_backend () {
	cat >"$TRASH_DIRECTORY/diff-process-backend.py" <<-\PYEOF
	import sys, os

	def read_pkt():
	    hdr = sys.stdin.buffer.read(4)
	    if len(hdr) < 4: return None
	    length = int(hdr, 16)
	    if length == 0: return ""
	    data = sys.stdin.buffer.read(length - 4)
	    return data.decode().rstrip("\n")

	def write_pkt(line):
	    data = (line + "\n").encode()
	    sys.stdout.buffer.write(f"{len(data)+4:04x}".encode() + data)
	    sys.stdout.buffer.flush()

	def write_flush():
	    sys.stdout.buffer.write(b"0000")
	    sys.stdout.buffer.flush()

	def read_content():
	    chunks = []
	    while True:
	        hdr = sys.stdin.buffer.read(4)
	        if len(hdr) < 4: break
	        length = int(hdr, 16)
	        if length == 0: break
	        chunks.append(sys.stdin.buffer.read(length - 4))
	    return b"".join(chunks)

	mode = "whole-file"
	logfile = None
	for arg in sys.argv[1:]:
	    if arg.startswith("--mode="):
	        mode = arg[7:]
	    elif arg.startswith("--log="):
	        logfile = open(arg[6:], "a")

	def log(msg):
	    if logfile:
	        logfile.write(msg + "\n")
	        logfile.flush()

	# Handshake
	assert read_pkt() == "git-diff-client"
	assert read_pkt() == "version=1"
	read_pkt()
	write_pkt("git-diff-server")
	write_pkt("version=1")
	write_flush()
	while True:
	    p = read_pkt()
	    if p == "": break
	write_pkt("capability=hunks")
	write_flush()

	log("ready")

	while True:
	    cmd = None
	    pathname = None
	    while True:
	        p = read_pkt()
	        if p is None: sys.exit(0)
	        if p == "": break
	        if p.startswith("command="): cmd = p.split("=",1)[1]
	        if p.startswith("pathname="): pathname = p.split("=",1)[1]
	    if cmd is None: sys.exit(0)
	    old = read_content()
	    new = read_content()
	    log(f"command={cmd} pathname={pathname}")

	    if mode == "error":
	        write_flush()
	        write_pkt("status=error")
	        write_flush()
	        continue

	    if mode == "abort":
	        write_flush()
	        write_pkt("status=abort")
	        write_flush()
	        continue

	    if mode == "crash":
	        sys.exit(1)

	    if cmd == "hunks":
	        if mode == "fixed-hunk":
	            write_pkt("hunk 5 2 5 2")
	        elif mode == "bad-hunk":
	            write_pkt("hunk 999 1 999 1")
	        elif mode == "bad-sync":
	            write_pkt("hunk 1 2 1 1")
	        elif mode == "overlap":
	            write_pkt("hunk 1 5 1 5")
	            write_pkt("hunk 3 2 3 2")
	        elif mode == "zero-hunk":
	            pass
	        else:
	            ol = old.count(b"\n")
	            nl = new.count(b"\n")
	            write_pkt(f"hunk 1 {ol} 1 {nl}")
	        write_flush()
	        write_pkt("status=success")
	        write_flush()
	    else:
	        write_flush()
	        write_pkt("status=error")
	        write_flush()
	PYEOF
	write_script diff-process-backend <<-SHEOF
	exec "$PYTHON_PATH" "$TRASH_DIRECTORY/diff-process-backend.py" "\$@"
	SHEOF
}

BACKEND="./diff-process-backend"

test_expect_success PYTHON 'setup' '
	setup_backend &&
	echo "*.c diff=cdiff" >.gitattributes &&
	git add .gitattributes &&
	git commit -m "initial"
'

test_expect_success PYTHON 'diff process hunk boundaries affect output' '
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
	git commit -m "add boundary.c" &&

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

test_expect_success PYTHON 'diff process fallback on tool error status' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=error --log=backend.log" \
		diff boundary.c >actual 2>stderr &&
	# Fallback produces the full builtin diff (both change regions).
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual &&
	# Tool was contacted (it replied with error, not crash).
	test_grep "command=hunks pathname=boundary.c" backend.log &&
	test_grep "diff process.*failed" stderr
'

test_expect_success PYTHON 'diff process fallback on bad hunks' '
	git -c diff.cdiff.process="$BACKEND --mode=bad-hunk" \
		diff boundary.c >actual &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual
'

test_expect_success PYTHON 'diff process fallback on tool crash' '
	git -c diff.cdiff.process="$BACKEND --mode=crash" \
		diff boundary.c >actual &&
	test_grep "^-OLD5" actual &&
	test_grep "^+NEW5" actual &&
	test_grep "^-OLD9" actual &&
	test_grep "^+NEW9" actual
'

test_expect_success PYTHON 'diff process abort disables for session' '
	cat >abort1.c <<-\EOF &&
	int first(void) { return 1; }
	EOF
	cat >abort2.c <<-\EOF &&
	int second(void) { return 2; }
	EOF
	git add abort1.c abort2.c &&
	git commit -m "add abort files" &&

	cat >abort1.c <<-\EOF &&
	int first(void) { return 10; }
	EOF
	cat >abort2.c <<-\EOF &&
	int second(void) { return 20; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=abort --log=backend.log" \
		diff -- abort1.c abort2.c >actual &&
	# Both files should still produce diff output via fallback.
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	# The tool aborts on the first file and git clears its
	# capability.  The second file never contacts the tool,
	# so the log should have exactly one entry, not two.
	test_grep "command=hunks" backend.log >matches &&
	test_line_count = 1 matches
'

test_expect_success PYTHON 'diff process error keeps tool available for next file' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=error --log=backend.log" \
		diff -- abort1.c abort2.c >actual 2>stderr &&
	# Unlike abort, error keeps the tool available: both files
	# are sent to the tool (and both fall back).
	test_grep "pathname=abort1.c" backend.log &&
	test_grep "pathname=abort2.c" backend.log &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual
'

test_expect_success PYTHON 'diff process handles multiple files' '
	cat >multi1.c <<-\EOF &&
	int one(void) { return 1; }
	EOF
	cat >multi2.c <<-\EOF &&
	int two(void) { return 2; }
	EOF
	git add multi1.c multi2.c &&
	git commit -m "add multi files" &&

	cat >multi1.c <<-\EOF &&
	int one(void) { return 10; }
	EOF
	cat >multi2.c <<-\EOF &&
	int two(void) { return 20; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- multi1.c multi2.c >actual 2>stderr &&
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	test_grep "pathname=multi1.c" backend.log &&
	test_grep "pathname=multi2.c" backend.log &&
	# No fallback warnings means hunks were accepted.
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process with --word-diff' '
	cat >worddiff.c <<-\EOF &&
	int value(void) { return 1; }
	EOF
	git add worddiff.c &&
	git commit -m "add worddiff.c" &&

	cat >worddiff.c <<-\EOF &&
	int value(void) { return 999; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --word-diff worddiff.c >actual 2>stderr &&
	test_grep "\[-1;-\]" actual &&
	test_grep "{+999;+}" actual &&
	# Verify the tool was contacted and no fallback occurred.
	test_grep "pathname=worddiff.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process bypassed by --diff-algorithm' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --diff-algorithm=patience worddiff.c >actual &&
	test_grep "return 999" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process not used by --stat' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --stat worddiff.c >actual &&
	test_grep "worddiff.c" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process bypassed by --no-ext-diff' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --no-ext-diff worddiff.c >actual &&
	test_grep "return 999" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process works with git log -p' '
	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 1; }
	EOF
	git add logtest.c &&
	git commit -m "add logtest.c" &&

	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 2; }
	EOF
	git add logtest.c &&
	git commit -m "change logtest.c" &&

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		log -1 -p -- logtest.c >actual 2>stderr &&
	test_grep "return 2" actual &&
	test_grep "command=hunks pathname=logtest.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process not used by format-patch' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		format-patch -1 --stdout -- logtest.c >actual &&
	test_grep "return 2" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process startup failure only warns once' '
	git -c diff.cdiff.process="/nonexistent/tool" \
		diff -- multi1.c multi2.c >actual 2>stderr &&
	# Both files produce diff output via fallback.
	test_grep "return 10" actual &&
	test_grep "return 20" actual &&
	# Sentinel prevents repeated warnings: only one, not one per file.
	test_grep "diff process.*failed" stderr >warnings &&
	test_line_count = 1 warnings
'

test_expect_success PYTHON 'diff process no hunks suppresses diff output' '
	cat >zerohunk.c <<-\EOF &&
	int zero(void) { return 0; }
	EOF
	git add zerohunk.c &&
	git commit -m "add zerohunk.c" &&

	cat >zerohunk.c <<-\EOF &&
	int zero(void) { return 999; }
	EOF

	git -c diff.cdiff.process="$BACKEND --mode=zero-hunk" \
		diff zerohunk.c >actual &&
	test_must_be_empty actual
'

test_expect_success PYTHON 'diff process no hunks with --exit-code returns success' '
	git -c diff.cdiff.process="$BACKEND --mode=zero-hunk" \
		diff --exit-code zerohunk.c
'

test_expect_success PYTHON 'diff process works with new file' '
	cat >newfile.c <<-\EOF &&
	int new_func(void) { return 42; }
	EOF
	git add newfile.c &&
	git commit -m "add newfile.c" &&

	cat >newfile.c <<-\EOF &&
	int new_func(void) { return 99; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- newfile.c >actual 2>stderr &&
	test_grep "return 99" actual &&
	test_grep "pathname=newfile.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process skipped for binary files' '
	printf "\\0binary" >binary.c &&
	git add binary.c &&
	git commit -m "add binary" &&
	printf "\\0changed" >binary.c &&

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- binary.c >actual &&
	test_grep "Binary files" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process with --exit-code and hunks returns failure' '
	test_expect_code 1 git -c diff.cdiff.process="$BACKEND" \
		diff --exit-code newfile.c
'

test_expect_success PYTHON 'diff process fallback on mismatched unchanged totals' '
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
	test_grep "changed" actual
'

test_expect_success PYTHON 'diff process fallback on overlapping hunks' '
	git -c diff.cdiff.process="$BACKEND --mode=overlap" \
		diff synctest.c >actual 2>stderr &&
	test_grep "changed" actual
'

test_expect_success PYTHON 'diff process works with added file (empty old side)' '
	cat >added.c <<-\EOF &&
	int added(void) { return 1; }
	EOF
	git add added.c &&

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --cached -- added.c >actual 2>stderr &&
	test_grep "added" actual &&
	test_grep "pathname=added.c" backend.log &&
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process not consulted for unmatched driver' '
	echo "not tracked by cdiff" >unmatched.txt &&
	git add unmatched.txt &&
	git commit -m "add unmatched.txt" &&

	echo "modified" >unmatched.txt &&

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- unmatched.txt >actual &&
	test_grep "modified" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'multiple drivers use separate processes' '
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

	rm -f backend-c.log backend-h.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend-c.log" \
	    -c diff.hdiff.process="$BACKEND --log=backend-h.log" \
		diff -- newfile.c multi.h >actual 2>stderr &&
	test_grep "pathname=newfile.c" backend-c.log &&
	test_grep "pathname=multi.h" backend-h.log &&
	test_must_be_empty stderr
'

test_expect_success PYTHON 'diff process works alongside textconv' '
	write_script uppercase-filter <<-\EOF &&
	tr "a-z" "A-Z"
	EOF

	cat >textconv.c <<-\EOF &&
	hello world
	EOF
	git add textconv.c &&
	git commit -m "add textconv.c" &&

	cat >textconv.c <<-\EOF &&
	goodbye world
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.textconv="./uppercase-filter" \
	    -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- textconv.c >actual 2>stderr &&
	# The diff process is consulted even with textconv active.
	test_grep "pathname=textconv.c" backend.log &&
	test_must_be_empty stderr
'

test_done
