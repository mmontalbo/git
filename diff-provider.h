#ifndef DIFF_PROVIDER_H
#define DIFF_PROVIDER_H

#include "xdiff-interface.h"

/*
 * The seam between naming a pair of file versions to diff and
 * computing their changed line ranges.  Consumers that operate on
 * hunk coordinates route their diff through here.
 *
 * A hunk provider answers a consumer's request from the pair's blob
 * object ids and the settings that determine the diff, before any
 * content is loaded; a request no provider answers falls through to
 * the consumer's own computation.  The diff-hunks store (diff-hunks.h)
 * is the provider consulted today.
 */

struct object_id;
struct repository;

/*
 * Nonzero when a hunk provider is available for the repository, for
 * consumers that decide up front whether consulting can pay off.
 */
int diff_provider_active(struct repository *r);

/*
 * Consult hunk providers for the changed ranges of the blob pair
 * (old_oid, new_oid) diffed under xdl_opts, without loading content.
 * On a hit the ranges are emitted through hunk_cb and 1 is returned;
 * nothing is emitted before the provider's answer is validated, so a
 * consumer may accumulate directly into its result.  0 is a miss: no
 * provider answered, and the consumer computes its diff as it would
 * without providers.
 */
int diff_provider_query_hunks(struct repository *r,
			      const struct object_id *old_oid,
			      const struct object_id *new_oid,
			      int xdl_opts,
			      xdl_emit_hunk_consume_func_t hunk_cb,
			      void *cb_data);

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
 * Emit the exact changed ranges (context 0) for the pair (old_oid,
 * new_oid) to hunk_cb.  Providers are consulted first, keyed by the
 * object ids and xpp's flags; pass NULL object ids when the diffed
 * bytes are not those blobs (or there are no blobs), which makes
 * every consult miss.  On a miss, fill supplies the content and
 * xdiff computes the ranges from xpp.  Returns 1 when a provider
 * supplied the ranges, 0 when they were computed, and -1 on failure
 * to load or diff.
 */
int diff_provider_emit_hunks(struct repository *r,
			     const struct object_id *old_oid,
			     const struct object_id *new_oid,
			     const xpparam_t *xpp,
			     hunk_pair_fill_fn fill, void *fill_data,
			     xdl_emit_hunk_consume_func_t hunk_cb,
			     void *cb_data);

#endif /* DIFF_PROVIDER_H */
