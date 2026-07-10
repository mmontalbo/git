#include "unit-test.h"
#include "range-set.h"

/* Assert that 'rs' holds exactly the n given [start, end) ranges. */
static void assert_ranges(struct range_set *rs, const long *expect, size_t n)
{
	size_t i;

	cl_assert_equal_i((int)rs->nr, (int)n);
	for (i = 0; i < n; i++) {
		cl_assert_equal_i((int)rs->ranges[i].start, (int)expect[2 * i]);
		cl_assert_equal_i((int)rs->ranges[i].end, (int)expect[2 * i + 1]);
	}
}

void test_range_set__sort_and_merge(void)
{
	struct range_set rs;
	const long expect[] = { 1, 4, 5, 8 };

	/* Out-of-order and overlapping input is canonicalized. */
	range_set_init(&rs, 0);
	range_set_append_unsafe(&rs, 5, 8);
	range_set_append_unsafe(&rs, 1, 3);
	range_set_append_unsafe(&rs, 2, 4);	/* overlaps [1,3) */
	sort_and_merge_range_set(&rs);

	assert_ranges(&rs, expect, 2);		/* [1,4), [5,8) */
	range_set_release(&rs);
}

void test_range_set__union(void)
{
	struct range_set a, b, out;
	const long expect[] = { 1, 5, 7, 9 };

	range_set_init(&a, 0);
	range_set_init(&b, 0);
	range_set_init(&out, 0);
	range_set_append(&a, 1, 3);
	range_set_append(&a, 7, 9);
	range_set_append(&b, 3, 5);		/* adjacent to [1,3), merges */
	range_set_union(&out, &a, &b);

	assert_ranges(&out, expect, 2);		/* [1,5), [7,9) */
	range_set_release(&a);
	range_set_release(&b);
	range_set_release(&out);
}

static void free_touched(struct diff_ranges *touched)
{
	range_set_release(&touched->parent);
	range_set_release(&touched->target);
	free(touched);
}

void test_range_set__map_untouched_range_shifts(void)
{
	/*
	 * A range the commit did not touch shifts onto the parent by the
	 * lines the diff added or removed before it, and nothing is reported
	 * as touched.  Here the commit inserts two lines (parent [1,1) ->
	 * target [1,3)), so lines after them map two lower and a tracked
	 * [5,8) becomes [3,6):
	 *
	 *   target:  0 1 2 3 4 5 6 7      tracked [5,8) = lines 5,6,7
	 *   parent:  0 . . 1 2 3 4 5      map to parent 3,4,5 = [3,6)
	 */
	struct range_set rs, out;
	struct diff_ranges diff;
	struct diff_ranges *touched = NULL;
	const long expect[] = { 3, 6 };

	range_set_init(&rs, 0);
	range_set_append(&rs, 5, 8);

	range_set_init(&diff.parent, 0);
	range_set_init(&diff.target, 0);
	range_set_append(&diff.parent, 1, 1);	/* 0 parent lines */
	range_set_append(&diff.target, 1, 3);	/* 2 inserted lines */

	range_set_init(&out, 0);
	range_set_map_across_diff(&out, &rs, &diff, &touched);

	assert_ranges(&out, expect, 1);
	cl_assert_equal_i((int)touched->target.nr, 0);

	range_set_release(&rs);
	range_set_release(&out);
	range_set_release(&diff.parent);
	range_set_release(&diff.target);
	free_touched(touched);
}

void test_range_set__map_touched_range_blames_commit(void)
{
	/*
	 * A range the commit changed is blamed on it: the range is replaced
	 * by its parent-side counterpart.  Here the commit rewrote target
	 * [2,4) (from parent [2,3)); tracking exactly [2,4) maps to the
	 * parent side [2,3), and [2,4) is recorded as touched.
	 */
	struct range_set rs, out;
	struct diff_ranges diff;
	struct diff_ranges *touched = NULL;
	const long expect_out[] = { 2, 3 };
	const long expect_touched[] = { 2, 4 };

	range_set_init(&rs, 0);
	range_set_append(&rs, 2, 4);

	range_set_init(&diff.parent, 0);
	range_set_init(&diff.target, 0);
	range_set_append(&diff.parent, 2, 3);
	range_set_append(&diff.target, 2, 4);

	range_set_init(&out, 0);
	range_set_map_across_diff(&out, &rs, &diff, &touched);

	assert_ranges(&out, expect_out, 1);
	assert_ranges(&touched->target, expect_touched, 1);

	range_set_release(&rs);
	range_set_release(&out);
	range_set_release(&diff.parent);
	range_set_release(&diff.target);
	free_touched(touched);
}

void test_range_set__union_disjoint(void)
{
	struct range_set a, b, out;
	const long expect[] = { 1, 3, 4, 6, 7, 9 };

	range_set_init(&a, 0);
	range_set_init(&b, 0);
	range_set_init(&out, 0);
	range_set_append(&a, 1, 3);
	range_set_append(&a, 7, 9);
	range_set_append(&b, 4, 6);		/* disjoint from both of a's */
	range_set_union(&out, &a, &b);

	assert_ranges(&out, expect, 3);		/* [1,3), [4,6), [7,9) */
	range_set_release(&a);
	range_set_release(&b);
	range_set_release(&out);
}

void test_range_set__union_overlap(void)
{
	struct range_set a, b, out;
	const long expect[] = { 1, 6 };

	range_set_init(&a, 0);
	range_set_init(&b, 0);
	range_set_init(&out, 0);
	range_set_append(&a, 1, 4);
	range_set_append(&b, 2, 6);		/* overlaps [1,4), merges */
	range_set_union(&out, &a, &b);

	assert_ranges(&out, expect, 1);		/* [1,6) */
	range_set_release(&a);
	range_set_release(&b);
	range_set_release(&out);
}

void test_range_set__sort_and_merge_contained(void)
{
	struct range_set rs;
	const long expect[] = { 1, 10 };

	/* A range fully inside an earlier one is absorbed. */
	range_set_init(&rs, 0);
	range_set_append_unsafe(&rs, 1, 10);
	range_set_append_unsafe(&rs, 3, 5);
	sort_and_merge_range_set(&rs);

	assert_ranges(&rs, expect, 1);		/* [1,10) */
	range_set_release(&rs);
}

void test_range_set__sort_and_merge_adjacent(void)
{
	struct range_set rs;
	const long expect[] = { 1, 5 };

	/* Adjacent (touching) ranges coalesce; a gap of one would not. */
	range_set_init(&rs, 0);
	range_set_append_unsafe(&rs, 3, 5);
	range_set_append_unsafe(&rs, 1, 3);	/* [1,3) touches [3,5) */
	sort_and_merge_range_set(&rs);

	assert_ranges(&rs, expect, 1);		/* [1,5) */
	range_set_release(&rs);
}

void test_range_set__sort_and_merge_drops_empty(void)
{
	struct range_set rs;
	const long expect[] = { 5, 8 };

	/* An empty [n,n) range is dropped. */
	range_set_init(&rs, 0);
	range_set_append_unsafe(&rs, 3, 3);
	range_set_append_unsafe(&rs, 5, 8);
	sort_and_merge_range_set(&rs);

	assert_ranges(&rs, expect, 1);		/* [5,8) */
	range_set_release(&rs);
}

void test_range_set__map_empty_set(void)
{
	/*
	 * An empty tracked set maps to nothing and touches nothing, even
	 * against a non-empty diff.  This also guards the boundary where
	 * diff_ranges_filter_touched() would otherwise index an empty set.
	 */
	struct range_set rs, out;
	struct diff_ranges diff;
	struct diff_ranges *touched = NULL;

	range_set_init(&rs, 0);

	range_set_init(&diff.parent, 0);
	range_set_init(&diff.target, 0);
	range_set_append(&diff.parent, 1, 1);
	range_set_append(&diff.target, 1, 3);

	range_set_init(&out, 0);
	range_set_map_across_diff(&out, &rs, &diff, &touched);

	cl_assert_equal_i((int)out.nr, 0);
	cl_assert_equal_i((int)touched->target.nr, 0);

	range_set_release(&rs);
	range_set_release(&out);
	range_set_release(&diff.parent);
	range_set_release(&diff.target);
	free_touched(touched);
}

void test_range_set__map_mixed_touched_and_untouched(void)
{
	/*
	 * One tracked range overlaps a hunk and one does not.  The touched
	 * range is blamed on the commit (replaced by its parent side), and
	 * the untouched range is shifted by the hunk's net line change.  Here
	 * the commit rewrote target [2,4) from parent [2,3) (net -1 line), so
	 * the untouched [6,8) maps to [5,7).
	 */
	struct range_set rs, out;
	struct diff_ranges diff;
	struct diff_ranges *touched = NULL;
	const long expect_out[] = { 2, 3, 5, 7 };
	const long expect_touched[] = { 2, 4 };

	range_set_init(&rs, 0);
	range_set_append(&rs, 2, 4);		/* touched */
	range_set_append(&rs, 6, 8);		/* untouched, after the hunk */

	range_set_init(&diff.parent, 0);
	range_set_init(&diff.target, 0);
	range_set_append(&diff.parent, 2, 3);
	range_set_append(&diff.target, 2, 4);

	range_set_init(&out, 0);
	range_set_map_across_diff(&out, &rs, &diff, &touched);

	assert_ranges(&out, expect_out, 2);		/* [2,3), [5,7) */
	assert_ranges(&touched->target, expect_touched, 1);

	range_set_release(&rs);
	range_set_release(&out);
	range_set_release(&diff.parent);
	range_set_release(&diff.target);
	free_touched(touched);
}
