#!/bin/sh

test_description='precomputed diff hunks for blame acceleration'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

DIFF_HUNKS_DIR=.git/objects/diff-hunks

# Helper: find first cache file inside sharded directory
find_first_cache_file () {
	find "$DIFF_HUNKS_DIR" -type f | head -1
}

test_expect_success 'setup' '
	test_commit initial file.txt "line 1" &&
	test_commit second file.txt "line 1
line 2" &&
	test_commit third file.txt "line 1
line 2
line 3" &&
	test_commit fourth file.txt "changed line 1
line 2
line 3
line 4"
'

test_expect_success 'git diff-hunks write --reachable creates per-path files' '
	git diff-hunks write --reachable &&
	test_path_is_dir $DIFF_HUNKS_DIR &&
	find $DIFF_HUNKS_DIR -type f >path_files &&
	test_line_count -ge 1 path_files
'

test_expect_success 'per-path file has correct DHPF signature' '
	file=$(find_first_cache_file) &&
	printf "DHPF" >expect &&
	test_copy_bytes 4 <$file >actual &&
	test_cmp expect actual
'

test_expect_success 'blame with precomputed hunks matches without' '
	git diff-hunks write --reachable &&
	git blame file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'precomputed hunks are actually used' '
	git diff-hunks write --reachable &&
	git blame --show-stats file.txt >output 2>&1 &&
	test_grep "num precomputed hits: [1-9][0-9]*" output
'

test_expect_success 'blame porcelain output matches with precomputed hunks' '
	rm -rf $DIFF_HUNKS_DIR &&
	git blame --porcelain file.txt >without_hunks &&
	git diff-hunks write --reachable &&
	git blame --porcelain file.txt >with_hunks &&
	test_cmp without_hunks with_hunks
'

test_expect_success 'blame falls back gracefully when files are absent' '
	rm -rf $DIFF_HUNKS_DIR &&
	git blame file.txt >actual &&
	test_line_count -ge 1 actual
'

test_expect_success 'blame handles merge commits' '
	git checkout -b side main~2 &&
	test_commit side-change side.txt "side content" &&
	git checkout main &&
	git merge --no-edit side &&
	git diff-hunks write --reachable &&
	git blame side.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame side.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame with -w falls back (xdl_opts mismatch)' '
	git diff-hunks write --reachable &&
	git blame -w file.txt >with_w &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame -w file.txt >without_w &&
	test_cmp with_w without_w
'

test_expect_success 'blame with diff.indentHeuristic=false falls back' '
	git diff-hunks write --reachable &&
	git -c diff.indentHeuristic=false blame file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git -c diff.indentHeuristic=false blame file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame with multiple files' '
	test_commit other other.txt "other content" &&
	test_commit modify-both file.txt "new line 1
line 2
line 3
line 4" &&
	git diff-hunks write --reachable &&
	git blame other.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame other.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'truncated per-path file is rejected' '
	git diff-hunks write --path file.txt &&
	file=$(find_first_cache_file) &&
	test_copy_bytes 32 <$file >truncated &&
	mv truncated $file &&
	git blame file.txt >actual &&
	test_line_count -ge 1 actual
'

test_expect_success 'corrupted signature is rejected' '
	git diff-hunks write --path file.txt &&
	file=$(find_first_cache_file) &&
	printf "XXXX" >corrupt &&
	tail -c +5 <$file >>corrupt &&
	mv corrupt $file &&
	git blame file.txt >actual &&
	test_line_count -ge 1 actual
'

test_expect_success 'regeneration overwrites old file' '
	git diff-hunks write --path file.txt &&
	file=$(find_first_cache_file) &&
	cp $file old &&
	test_commit another file.txt "yet another change" &&
	git diff-hunks write --path file.txt &&
	! test_cmp old $file
'

test_expect_success 'diff-hunks write requires --reachable or --path' '
	test_must_fail git diff-hunks write 2>err &&
	test_grep "only --reachable or --path" err
'

test_expect_success '--path and --reachable are mutually exclusive' '
	test_must_fail git diff-hunks write --reachable --path file.txt 2>err &&
	test_grep "cannot be used together" err
'

test_expect_success 'blame --incremental with precomputed hunks' '
	git diff-hunks write --reachable &&
	git blame --incremental file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame --incremental file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame --reverse with precomputed hunks' '
	git diff-hunks write --reachable &&
	git blame --reverse HEAD~3..HEAD file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame --reverse HEAD~3..HEAD file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame -M with precomputed hunks' '
	cp file.txt moved.txt &&
	git add moved.txt &&
	git commit -m "copy file" &&
	git diff-hunks write --reachable &&
	git blame -M moved.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame -M moved.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame -C produces correct output with diff-hunks file present' '
	git diff-hunks write --reachable &&
	git blame -C moved.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame -C moved.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'blame --show-stats reports precomputed hits' '
	git diff-hunks write --reachable &&
	git blame --show-stats file.txt >output 2>&1 &&
	test_grep "num precomputed hits" output
'

test_expect_success 'blame with many-hunk commit matches without hunks' '
	# Create a file with 2002 lines, then change every other line
	# to produce >1000 separate hunks (stress test for large diffs)
	test_seq 1 2002 >large.txt &&
	git add large.txt &&
	git commit -m "add large file" &&
	awk "{if (NR % 2 == 1) print \"changed \" \$0; else print}" \
		<large.txt >large.tmp &&
	mv large.tmp large.txt &&
	git add large.txt &&
	git commit -m "modify every other line" &&
	git diff-hunks write --reachable &&
	git blame large.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame large.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'empty repo handles diff-hunks write gracefully' '
	git init empty &&
	(
		cd empty &&
		git diff-hunks write --reachable
	)
'

test_expect_success 'binary file does not break diff-hunks' '
	printf "\0binary\0content" >binary.bin &&
	git add binary.bin &&
	git commit -m "add binary" &&
	printf "\0changed\0binary" >binary.bin &&
	git add binary.bin &&
	git commit -m "modify binary" &&
	git diff-hunks write --reachable &&
	git blame binary.bin >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame binary.bin >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'mode-only change does not break diff-hunks' '
	test_chmod +x file.txt &&
	git commit -m "make executable" &&
	git diff-hunks write --reachable &&
	git blame file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success '--path and --reachable produce equivalent blame results' '
	rm -rf $DIFF_HUNKS_DIR &&
	git diff-hunks write --path file.txt &&
	git blame file.txt >with_path &&
	rm -rf $DIFF_HUNKS_DIR &&
	git diff-hunks write --reachable &&
	git blame file.txt >with_reachable &&
	test_cmp with_path with_reachable
'

test_expect_success 'merge commit with both parents modifying same file' '
	git checkout -b merge-base2 main~3 &&
	test_seq 1 10 >merge-file.txt &&
	git add merge-file.txt &&
	git commit -m "add merge-file base" &&
	git checkout -b merge-left3 &&
	sed "s/^1$/left/" merge-file.txt >tmp && mv tmp merge-file.txt &&
	git add merge-file.txt &&
	git commit -m "left modifies line 1" &&
	git checkout merge-base2 &&
	git checkout -b merge-right3 &&
	sed "s/^10$/right/" merge-file.txt >tmp && mv tmp merge-file.txt &&
	git add merge-file.txt &&
	git commit -m "right modifies line 10" &&
	git checkout main &&
	git merge --no-edit merge-left3 merge-right3 &&
	git diff-hunks write --reachable &&
	git blame merge-file.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame merge-file.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'deep history blame with --path matches without' '
	# Generate enough commits that file has substantial history
	# (exercises tail of hunk data where checksum truncation would bite)
	for i in $(test_seq 1 50)
	do
		echo "deep history line $i" >>deep.txt &&
		git add deep.txt &&
		git commit -m "deep commit $i" || return 1
	done &&
	git diff-hunks write --path deep.txt &&
	git blame deep.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame deep.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'deep history blame with --reachable matches without' '
	git diff-hunks write --reachable &&
	git blame deep.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame deep.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_expect_success 'path containing double dash does not collide' '
	mkdir -p a &&
	echo "content for a/b.c" >a/b.c &&
	echo "content for a--b.c" >a--b.c &&
	git add a/b.c a--b.c &&
	git commit -m "add paths that could collide" &&
	echo "changed a/b.c" >a/b.c &&
	echo "changed a--b.c" >a--b.c &&
	git add a/b.c a--b.c &&
	git commit -m "modify both" &&
	git diff-hunks write --path a/b.c &&
	git diff-hunks write --path a--b.c &&
	git blame a/b.c >with_hunks_slash &&
	git blame a--b.c >with_hunks_dash &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame a/b.c >without_hunks_slash &&
	git blame a--b.c >without_hunks_dash &&
	test_cmp with_hunks_slash without_hunks_slash &&
	test_cmp with_hunks_dash without_hunks_dash
'

test_expect_success 'diff-hunks clear removes cache directory' '
	git diff-hunks write --reachable &&
	test_path_is_dir $DIFF_HUNKS_DIR &&
	git diff-hunks clear &&
	test_path_is_missing $DIFF_HUNKS_DIR
'

test_expect_success 'diff-hunks clear on nonexistent directory succeeds' '
	test_path_is_missing $DIFF_HUNKS_DIR &&
	git diff-hunks clear
'

test_expect_success 'blame across rename with precomputed hunks' '
	echo "original content" >rename-src.txt &&
	git add rename-src.txt &&
	git commit -m "add rename-src" &&
	echo "modified content" >>rename-src.txt &&
	git add rename-src.txt &&
	git commit -m "modify rename-src" &&
	git mv rename-src.txt rename-dst.txt &&
	git commit -m "rename src to dst" &&
	echo "post-rename change" >>rename-dst.txt &&
	git add rename-dst.txt &&
	git commit -m "modify after rename" &&
	git diff-hunks write --path rename-dst.txt &&
	git diff-hunks write --path rename-src.txt &&
	git blame rename-dst.txt >with_hunks &&
	rm -rf $DIFF_HUNKS_DIR &&
	git blame rename-dst.txt >without_hunks &&
	test_cmp with_hunks without_hunks
'

test_done
