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

struct object_id;
struct repository;

/*
 * Nonzero when the in-process provider, the diff-hunks store, is
 * present, for consumers that decide up front whether consulting can
 * pay off.  A configured process does not register here: its paths
 * are consulted regardless.
 */
int diff_provider_active(struct repository *r);

/*
 * Consult the providers that answer from this key alone (the
 * diff-hunks store) for the changed ranges of the blob pair
 * (old_oid, new_oid) diffed under xdl_opts, without loading content.
 * On a hit the ranges are emitted through hunk_cb and 1 is returned;
 * nothing is emitted before the provider's answer is validated, so a
 * consumer may accumulate directly into its result.  0 is a miss: no
 * provider answered, and the consumer computes its diff as it would
 * without providers.  The callback's return value is not consulted:
 * replay of a validated answer has no error leg, so the callback
 * must return 0.
 */
int diff_provider_query_hunks(struct repository *r,
			      const struct object_id *old_oid,
			      const struct object_id *new_oid,
			      int xdl_opts,
			      xdl_emit_hunk_consume_func_t hunk_cb,
			      void *cb_data);

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
