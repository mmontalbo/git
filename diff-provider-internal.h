#ifndef DIFF_PROVIDER_INTERNAL_H
#define DIFF_PROVIDER_INTERNAL_H

#include "diff-provider.h"

/*
 * The implementor-facing half of the hunk provider interface: the
 * rules a provider applies to its own answer before any consumer
 * sees it.  Provider implementations include this header; consumers
 * of the interface use only diff-provider.h.
 */

/*
 * Incremental well-formedness check for a provider-supplied hunk
 * sequence, shared by every provider.  Each coordinate, and each
 * hunk's end (its start plus count), must fit int32 (a consumer may
 * truncate to int, and a provider may serialize as such); hunks must
 * be in order and must not overlap; and the
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
diff_provider_check_hunk(struct diff_provider_hunks_check *c,
			  long old_start, long old_count,
			  long new_start, long new_count);

#endif /* DIFF_PROVIDER_INTERNAL_H */
