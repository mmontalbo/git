#ifndef DIFF_PROVIDER_H
#define DIFF_PROVIDER_H

#include "xdiff-interface.h"

/*
 * The hunk provider interface sits between naming a pair of file
 * versions to diff and computing their changed line ranges.
 * Consumers that operate on hunk coordinates route their diff
 * through here, so that a provider can answer for the pair before
 * its content is loaded.
 *
 * A hunk provider answers a consumer's request from the pair's
 * identity (its blob object ids) and the parameters that determine
 * the diff; a request no provider answers falls through to the
 * consumer's own computation.  A provider is either authoritative for
 * its requests, meaning its answer may deliberately differ from the
 * builtin diff, or not, meaning its answer must reproduce the builtin
 * result exactly.  Either way, an answer's coordinates must pass
 * the shared coordinate check (diff-provider-internal.h) before any
 * consumer sees them.
 */

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
