#include "git-compat-util.h"
#include "range-set.h"

static void range_set_grow(struct range_set *rs, size_t extra)
{
	ALLOC_GROW(rs->ranges, rs->nr + extra, rs->alloc);
}

void range_set_init(struct range_set *rs, size_t prealloc)
{
	rs->alloc = rs->nr = 0;
	rs->ranges = NULL;
	if (prealloc)
		range_set_grow(rs, prealloc);
}

void range_set_release(struct range_set *rs)
{
	FREE_AND_NULL(rs->ranges);
	rs->alloc = rs->nr = 0;
}

void range_set_copy(struct range_set *dst, struct range_set *src)
{
	range_set_init(dst, src->nr);
	COPY_ARRAY(dst->ranges, src->ranges, src->nr);
	dst->nr = src->nr;
}

void range_set_move(struct range_set *dst, struct range_set *src)
{
	range_set_release(dst);
	dst->ranges = src->ranges;
	dst->nr = src->nr;
	dst->alloc = src->alloc;
	src->ranges = NULL;
	src->alloc = src->nr = 0;
}

void range_set_append_unsafe(struct range_set *rs, long a, long b)
{
	assert(a <= b);
	range_set_grow(rs, 1);
	rs->ranges[rs->nr].start = a;
	rs->ranges[rs->nr].end = b;
	rs->nr++;
}

void range_set_append(struct range_set *rs, long a, long b)
{
	assert(rs->nr == 0 || rs->ranges[rs->nr-1].end <= a);
	range_set_append_unsafe(rs, a, b);
}

static int range_cmp(const void *_r, const void *_s)
{
	const struct range *r = _r;
	const struct range *s = _s;

	return (r->start > s->start) - (r->start < s->start);
}

/*
 * Check that the ranges are non-empty, sorted and non-overlapping
 */
static void range_set_check_invariants(struct range_set *rs)
{
	unsigned int i;

	if (rs->nr)
		assert(rs->ranges[0].start < rs->ranges[0].end);

	for (i = 1; i < rs->nr; i++) {
		assert(rs->ranges[i-1].end < rs->ranges[i].start);
		assert(rs->ranges[i].start < rs->ranges[i].end);
	}
}

/*
 * Append [start, end) to rs, coalescing it with the last range when they
 * overlap or touch, and dropping it when empty.  The caller must append in
 * nondecreasing start order, so rs stays sorted.  This is the shared
 * canonicalizing step for building a range set from an already-ordered
 * stream, whether into a fresh set or in place over the same buffer.
 */
static void range_set_append_coalesce(struct range_set *rs, long start, long end)
{
	if (start == end)
		return;
	if (!rs->nr || rs->ranges[rs->nr-1].end < start) {
		range_set_grow(rs, 1);
		rs->ranges[rs->nr].start = start;
		rs->ranges[rs->nr].end = end;
		rs->nr++;
	} else if (rs->ranges[rs->nr-1].end < end) {
		rs->ranges[rs->nr-1].end = end;
	}
}

void sort_and_merge_range_set(struct range_set *rs)
{
	unsigned int i, nr = rs->nr;

	QSORT(rs->ranges, nr, range_cmp);

	/*
	 * Compact in place.  The output cursor is rs->nr, reset to 0; it
	 * never overtakes the read index i, and range_set_grow() does not
	 * reallocate while nr stays within the original count, so appending
	 * over the same buffer never clobbers an unread range.
	 */
	rs->nr = 0;
	for (i = 0; i < nr; i++)
		range_set_append_coalesce(rs, rs->ranges[i].start,
					  rs->ranges[i].end);

	range_set_check_invariants(rs);
}

void range_set_union(struct range_set *out,
		     struct range_set *a, struct range_set *b)
{
	unsigned int i = 0, j = 0;
	struct range *ra = a->ranges;
	struct range *rb = b->ranges;

	assert(out->nr == 0);
	while (i < a->nr || j < b->nr) {
		struct range *new_range;
		if (i < a->nr && j < b->nr) {
			if (ra[i].start < rb[j].start)
				new_range = &ra[i++];
			else if (ra[i].start > rb[j].start)
				new_range = &rb[j++];
			else if (ra[i].end < rb[j].end)
				new_range = &ra[i++];
			else
				new_range = &rb[j++];
		} else if (i < a->nr)      /* b exhausted */
			new_range = &ra[i++];
		else                       /* a exhausted */
			new_range = &rb[j++];
		range_set_append_coalesce(out, new_range->start,
					  new_range->end);
	}
}

/*
 * Difference of two range sets: out = a \ b, the parts of a not covered by
 * any range in b (for example [1,10) \ {[3,5),[7,8)} = {[1,3),[5,7),[8,10)}).
 * a and b must be canonical (sorted, disjoint and non-empty), out empty and
 * distinct from both; an empty range in b covers nothing but would still
 * split an output range in two.
 *
 * In the diagrams below, "a" is the still-uncovered part of the current a
 * range, [start, end), and "b" is b->ranges[j]; neither is the whole set.
 */
static void range_set_difference(struct range_set *out,
				  struct range_set *a, struct range_set *b)
{
	unsigned int i, j = 0;
	for (i = 0; i < a->nr; i++) {
		long start = a->ranges[i].start;
		long end = a->ranges[i].end;
		while (start < end) {
			while (j < b->nr && start >= b->ranges[j].end)
				/*
				 * a:         |-------
				 * b: ------|
				 */
				j++;
			if (j >= b->nr || end <= b->ranges[j].start) {
				/*
				 * b exhausted, or
				 * a:  ----|
				 * b:         |----
				 */
				range_set_append(out, start, end);
				break;
			}
			if (start >= b->ranges[j].start) {
				/*
				 * a:     |--????
				 * b: |------|
				 */
				start = b->ranges[j].end;
			} else if (end > b->ranges[j].start) {
				/*
				 * a: |-----|
				 * b:    |--?????
				 */
				if (start < b->ranges[j].start)
					range_set_append(out, start, b->ranges[j].start);
				start = b->ranges[j].end;
			}
		}
	}
}

/* diff_ranges: a diff expressed as paired parent/target range sets. */

void diff_ranges_init(struct diff_ranges *diff)
{
	range_set_init(&diff->parent, 0);
	range_set_init(&diff->target, 0);
}

void diff_ranges_release(struct diff_ranges *diff)
{
	range_set_release(&diff->parent);
	range_set_release(&diff->target);
}

/*
 * Mapping a range set backward across a diff: the target side accounts
 * for its "+" (target-side) ranges, so a touched range is replaced by
 * its parent-side counterpart (range_set_map_across_diff).
 */

static int ranges_overlap(const struct range *a, const struct range *b)
{
	return !(a->end <= b->start || b->end <= a->start);
}

/*
 * Given a diff and the set of interesting ranges, determine all hunks
 * of the diff which touch (overlap) at least one of the interesting
 * ranges in the target.
 */
static void diff_ranges_filter_touched(struct diff_ranges *out,
				       struct diff_ranges *diff,
				       struct range_set *rs)
{
	unsigned int i, j = 0;

	assert(out->target.nr == 0);

	if (!rs->nr)
		return;	/* nothing is interesting, so nothing is touched */

	for (i = 0; i < diff->target.nr; i++) {
		while (diff->target.ranges[i].start >= rs->ranges[j].end) {
			j++;
			if (j == rs->nr)
				return;
		}
		if (ranges_overlap(&diff->target.ranges[i], &rs->ranges[j])) {
			range_set_append(&out->parent,
					 diff->parent.ranges[i].start,
					 diff->parent.ranges[i].end);
			range_set_append(&out->target,
					 diff->target.ranges[i].start,
					 diff->target.ranges[i].end);
		}
	}
}

/*
 * Adjust the line numbers in 'rs' by the net lines the diff added or removed
 * before each range, carrying them from the target side back to the parent.
 * For example, across a diff that inserted two lines above it, a target
 * range [5,8) becomes the parent range [3,6).
 */
static void range_set_shift_diff(struct range_set *out,
				 struct range_set *rs,
				 struct diff_ranges *diff)
{
	unsigned int i, j = 0;
	long offset = 0;
	struct range *src = rs->ranges;
	struct range *target = diff->target.ranges;
	struct range *parent = diff->parent.ranges;

	for (i = 0; i < rs->nr; i++) {
		while (j < diff->target.nr && src[i].start >= target[j].start) {
			offset += (parent[j].end-parent[j].start)
				- (target[j].end-target[j].start);
			j++;
		}
		range_set_append(out, src[i].start+offset, src[i].end+offset);
	}
}

/*
 * See range-set.h.  Collect the hunks that touch 'rs', remove their
 * target side from 'rs' and shift the untouched remainder across the
 * diff, then union in the parent side of the touched hunks.
 */
void range_set_map_across_diff(struct range_set *out,
			       struct range_set *rs,
			       struct diff_ranges *diff,
			       struct diff_ranges **touched_out)
{
	struct diff_ranges *touched = xmalloc(sizeof(*touched));
	struct range_set untouched = { 0 };
	struct range_set shifted = { 0 };

	diff_ranges_init(touched);
	diff_ranges_filter_touched(touched, diff, rs);
	range_set_difference(&untouched, rs, &touched->target);
	range_set_shift_diff(&shifted, &untouched, diff);
	range_set_union(out, &shifted, &touched->parent);
	range_set_release(&untouched);
	range_set_release(&shifted);

	*touched_out = touched;
}
