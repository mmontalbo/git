#ifndef DIFF_PROVIDER_H
#define DIFF_PROVIDER_H

#include "xdiff-interface.h"

/*
 * The seam between naming a pair of file versions to diff and
 * computing their changed line ranges.  Consumers that operate on
 * hunk coordinates route their diff through here.
 */

/*
 * Incremental well-formedness check for a provider-supplied hunk
 * sequence, shared by every provider.  Each coordinate must fit int32
 * (a consumer may truncate to int, and a provider may serialize as
 * such); hunks must be in order and must not overlap; and the
 * unchanged run between hunks must be the same length on both sides,
 * or a consumer that walks the two files in lockstep desynchronizes.
 * Every rule constrains differences between coordinates, so the check
 * applies to 0-based and 1-based sequences alike.
 *
 * Feed the hunks in order to a zero-initialized struct; the first
 * nonzero return names the violated rule, and the whole sequence must
 * then be discarded unemitted.
 */
struct diff_provider_hunks_check {
	int64_t prev_old_end, prev_new_end;
};

enum diff_provider_hunks_error {
	PROVIDER_HUNKS_OK = 0,
	PROVIDER_HUNKS_RANGE,      /* negative or beyond int32 */
	PROVIDER_HUNKS_OVERLAP,    /* out of order or overlapping */
	PROVIDER_HUNKS_MISALIGNED, /* unchanged runs differ in length */
};

enum diff_provider_hunks_error
diff_provider_hunks_check(struct diff_provider_hunks_check *c,
			  long old_start, long old_count,
			  long new_start, long new_count);

/*
 * Load the pair's content.  Called at most once per request, only
 * when the ranges are computed rather than provided.  The buffers
 * borrow storage owned by the callback's owner.
 */
typedef int (*hunk_pair_fill_fn)(void *data, mmfile_t *old_file,
				 mmfile_t *new_file);

/*
 * Emit the exact changed ranges (context 0) for one file pair to
 * hunk_cb.  xpp carries the diff parameters that determine the
 * ranges.  Returns 0 on success, -1 on failure to load or diff.
 */
int diff_provider_emit_hunks(const xpparam_t *xpp,
			     hunk_pair_fill_fn fill, void *fill_data,
			     xdl_emit_hunk_consume_func_t hunk_cb,
			     void *cb_data);

#endif /* DIFF_PROVIDER_H */
