#ifndef RANGE_SET_H
#define RANGE_SET_H

/*
 * A range_set is a sorted set of disjoint half-open line ranges
 * [start, end).  Besides the usual set algebra (union, difference), this
 * module provides range_set_map_across_diff(): given a set of lines of
 * interest in a file and that file's diff against its parent, it maps the
 * ranges back to the parent side and reports which ones the diff touched.
 *
 * That is how "git log -L" (line-log.c) and "git blame -L" follow a set
 * of lines through history: the ranges a commit changed are attributed to
 * it, and the rest are carried to the parent, shifted by the lines added
 * or removed above them.
 */

/* A half-open range [start, end) of line numbers, counting from 0. */
struct range {
	long start, end;
};

/* A set of ranges.  The ranges must always be disjoint and sorted. */
struct range_set {
	unsigned int alloc, nr;
	struct range *ranges;
};

/*
 * The line geometry of a diff between two file versions, with the changed
 * text discarded: an index-aligned list of hunks.  parent and target run
 * in parallel (parent.nr == target.nr); for hunk i, parent.ranges[i] is
 * the pre-image line range and target.ranges[i] the post-image range.
 */
struct diff_ranges {
	struct range_set parent;
	struct range_set target;
};

void range_set_init(struct range_set *, size_t prealloc);
void range_set_release(struct range_set *);
/*
 * dst must be uninitialized; unlike range_set_move(), range_set_copy()
 * does not release dst first.
 */
void range_set_copy(struct range_set *dst, struct range_set *src);
/* Move src into dst, releasing dst first and leaving src empty. */
void range_set_move(struct range_set *dst, struct range_set *src);
/* Append a range in any order; sort_and_merge_range_set() before use. */
void range_set_append_unsafe(struct range_set *, long start, long end);
/* New range must begin at or after end of last added range */
void range_set_append(struct range_set *, long start, long end);
/*
 * Sort the range set and merge overlapping or adjacent ranges in place,
 * dropping empty ranges, to establish the sorted, disjoint invariant
 * after ranges were added with range_set_append_unsafe().
 */
void sort_and_merge_range_set(struct range_set *);

/*
 * Union of two (sorted, disjoint) range sets.  The result is canonical:
 * overlapping and adjacent ranges are merged, and empty ranges are
 * removed.
 */
void range_set_union(struct range_set *out,
		     struct range_set *a, struct range_set *b);

void diff_ranges_init(struct diff_ranges *diff);
void diff_ranges_release(struct diff_ranges *diff);

/*
 * Map the ranges 'rs' backward across 'diff', from the target side to the
 * parent side, writing the result to 'out': a range the diff touched is
 * replaced by its parent-side counterpart, and an untouched range is
 * shifted by the lines the diff added or removed before it.  The touched
 * hunks are returned in *touched_out, which the caller releases with
 * diff_ranges_release() and then frees.
 */
void range_set_map_across_diff(struct range_set *out, struct range_set *rs,
			       struct diff_ranges *diff,
			       struct diff_ranges **touched_out);

#endif /* RANGE_SET_H */
