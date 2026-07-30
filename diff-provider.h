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
 * A hunk provider answers a consumer's request from the pair's blob
 * object ids and the settings that determine the diff, before any
 * content is loaded; a request no provider answers falls through to
 * the consumer's own computation.  Two providers implement this
 * interface with different authority.  The diff-hunks store
 * (diff-hunks.h) is in-process and not authoritative: it may only
 * reproduce the builtin result, so it never asserts a pair
 * equivalent, and it stands aside wherever a process outranks it.  A
 * process configured in diff.<driver>.process (diff-process.h) is
 * authoritative for its paths: its answer may deliberately differ
 * from the builtin diff, including asserting a pair equivalent.  The
 * consult order in diff_provider_emit_hunks() is that authority
 * resolution.  Whichever provider answers, its coordinates pass
 * the shared coordinate check (diff-provider-internal.h) before
 * any consumer sees them.
 *
 * Providers serve two consumer shapes.  A consumer that needs only
 * the coordinates is served outright: its callback receives the
 * provider's hunks and no diff runs.  A consumer that runs its own
 * emission over the file text (patch output) can have only its hunk
 * selection replaced: the process's answer rides to xdiff in xpparam_t's
 * external hunks (diff_process_fill_hunks()), and xdiff emits context
 * and text from the real content.
 */

struct diff_options;
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
 * Emit the exact changed ranges (context 0) for the pair (old_oid,
 * new_oid) at path to hunk_cb.
 *
 * The producer is picked before content is loaded.  A path whose
 * driver has a diff process makes the process the producer: the
 * store's entries hold xdiff's answer, which the process may
 * deliberately contradict, so the store is not consulted, and the
 * process is asked by the pair's object ids first.  A request the
 * identity phase does not settle loads content, through fill, and
 * asks the process with it; its hunks then feed xdiff's emission,
 * and a process that reports the pair equivalent emits no hunks at
 * all.  On a path with no process the store is consulted by the
 * object ids and xpp's flags, unless -I patterns or anchors are in
 * effect (they shape the diff outside the key); only a miss loads
 * content and computes.
 *
 * Pass NULL object ids when the diffed bytes are not those blobs (or
 * there are no blobs): the identity phase then misses, and no id is
 * sent to a process.  Pass a NULL diffopt or path to consult no
 * process.
 * Returns 1 when the store supplied the ranges, 0 when they were
 * emitted any other way, and -1 on failure to load or diff.
 */
int diff_provider_emit_hunks(struct repository *r,
			     const struct object_id *old_oid,
			     const struct object_id *new_oid,
			     const char *path,
			     struct diff_options *diffopt,
			     const xpparam_t *xpp,
			     hunk_pair_fill_fn fill, void *fill_data,
			     xdl_emit_hunk_consume_func_t hunk_cb,
			     void *cb_data);

#endif /* DIFF_PROVIDER_H */
