#ifndef DIFF_HUNKS_H
#define DIFF_HUNKS_H

#include "hash.h"

struct object_id;
struct repository;
struct object_database;

/*
 * A persistent store of precomputed diff hunk coordinates, at
 * .git/objects/diff-hunks. Entries are keyed by the two blobs diffed
 * and the diff settings they were diffed under (see struct
 * diff_hunks_settings), so a cached result is valid in any context that
 * key recurs in, independent of path.
 *
 * The store is a cache: ordinary commands read it and fall back to
 * computing the diff when it is absent, stale, or corrupt. It is filled
 * as a side effect of diff and log runs, but only when writing is
 * enabled; writing is off by default, so an ordinary command reads the
 * store without recording into it.
 */

/*
 * The diff settings a blob pair was diffed under: the subset of
 * diff_options that changes the resulting hunks. Together with the
 * blob pair, these key an entry: a stored result is served only to a
 * request whose settings match. Both mirror the (always non-negative)
 * diff_options fields they project from, and are serialized and compared
 * as 4-byte big-endian integers.
 *
 * The hunks a pair produces are not unique. They vary with the xdiff
 * algorithm and ignore flags (xdl_opts), and with whether the diff is
 * trimmed: a zero context triggers trim_common_tail, which can pick a
 * different but equally valid set of hunks than an untrimmed diff. The
 * context in the key selects the trimmed hunks git-blame reads or the
 * untrimmed hunks diffstat sums; the recorded coordinates are always at
 * zero context, so an entry keyed by a nonzero context holds the counts
 * diffstat sums, not that context's hunk boundaries. Distinct settings
 * that happen to produce identical hunks are not folded together, so a
 * lookup at settings the store was not built for misses and the diff is
 * recomputed.
 */
struct diff_hunks_settings {
	int xdl_opts;
	int context;
};

/*
 * A hunk's coordinates. The type is long to match the xdiff emit
 * callback; the values are a diff's line numbers and counts, always
 * within the int32 range the on-disk format stores (see
 * diff_hunks_writer_add()).
 */
struct precomputed_hunk {
	long old_start;
	long old_count;
	long new_start;
	long new_count;
};

/*
 * The repository's store, loaded once on first use and cached on the
 * object database. Returns NULL when reading is disabled
 * (core.diffHunks=false), the store is absent, or its object hash
 * differs. The lookup functions below accept a NULL store and treat it
 * as empty (every lookup misses), so callers need not check for NULL.
 * The object database owns the store; callers must not free it.
 */
struct diff_hunks_store *repo_diff_hunks_store(struct repository *r);

/* Free the repository's cached store, at object-database teardown. */
void close_diff_hunks_store(struct object_database *o);

/*
 * Sum the recorded hunk line counts for an (old blob, new blob) pair
 * under the settings ds into *added/*deleted; returns 1 on a hit, 0 on a
 * miss (the caller then computes the diff). This is the read side for
 * diffstat, which needs only the counts. A consumer that needs the hunk
 * coordinates uses diff_hunks_emit(), which validates them before replay.
 */
int diff_hunks_store_sum(struct diff_hunks_store *s,
			 const struct object_id *old_oid,
			 const struct object_id *new_oid,
			 const struct diff_hunks_settings *ds,
			 uintmax_t *added, uintmax_t *deleted);

/*
 * A warming run's writer: it accumulates the hunks it computes in memory
 * and flushes them to the store in one pass at finish.
 */
struct diff_hunks_writer;

/*
 * Return a writer for a warming run, or NULL when writing is disabled
 * (the default). diff_hunks_writer_add() tolerates a NULL writer, so a
 * caller may attach the result unconditionally. Pair with
 * diff_hunks_writer_finish().
 */
struct diff_hunks_writer *diff_hunks_writer_maybe_new(struct repository *r);

/*
 * Record a blob pair's hunks as computed under the settings ds; a later
 * lookup with matching settings is served these hunks. NULL-safe.
 */
void diff_hunks_writer_add(struct diff_hunks_writer *w,
			    const struct object_id *old_oid,
			    const struct object_id *new_oid,
			    const struct diff_hunks_settings *ds,
			    const struct precomputed_hunk *hunks,
			    size_t nr_hunks);

/* Flush the accumulated entries to the store and free the writer. NULL-safe. */
void diff_hunks_writer_finish(struct diff_hunks_writer *w);

/* Remove the store files (base and overlay). Returns 0 (incl. absent) or -1. */
int diff_hunks_clear(struct repository *r);
/* Validate the store (base and overlay). Returns 0 if valid/absent, -1 if corrupt. */
int diff_hunks_verify(struct repository *r);
/* Fold the overlay tier into the base and drop it. Returns 0 or -1. */
int diff_hunks_compact(struct repository *r);

#endif /* DIFF_HUNKS_H */
