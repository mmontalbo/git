#ifndef DIFF_PROCESS_H
#define DIFF_PROCESS_H

#include "xdiff/xdiff.h"

struct diff_options;
struct object_id;

/*
 * diff.<driver>.process support: consult a long-running external program for
 * which lines changed between two blobs, in place of the builtin diff.
 *
 * Two entry points serve two kinds of consumer:
 *   - diff_process_fill_hunks() for content consumers (patch, --stat): it
 *     sends both blobs and installs the returned hunks into an xpparam_t for a
 *     following xdi_diff()/xdi_diff_outf() to render.
 *   - diff_process_hunks_by_oid() for header-only consumers (blame,
 *     "git log -L" range tracking): it asks by object id with no blob loaded
 *     and drives a hunk callback from the answer.
 *
 * Both validate the returned hunks before use.  Git launches the configured
 * process on the first consult, caches it per userdiff driver, and reuses it
 * across files.  Git tears it down at exit.  Callers manage no process state.
 * A content consumer releases the hunks diff_process_fill_hunks() installed by
 * calling diff_process_clear_hunks(); the by-oid entry installs nothing.
 */

enum diff_process_result {
	DIFF_PROCESS_ERROR = -1,   /* process failed; caller falls back (warned) */
	DIFF_PROCESS_DIFFED = 0,   /* proceed: xpp holds the process's hunks, or none */
	DIFF_PROCESS_EQUIVALENT,   /* process reports the two blobs identical */
};

/*
 * Consult the diff process configured for 'path'.  On success it installs
 * the returned hunks into xpp for the following xdi_diff()/xdi_diff_outf()
 * to render; release them with diff_process_clear_hunks().  It uses
 * xpp->external_hunks exclusively and overwrites any prior value.
 *
 * Handles driver lookup, the flag checks that suppress the process
 * (--diff-algorithm), subprocess management,
 * and error reporting.
 *
 * Returns DIFF_PROCESS_DIFFED when the caller should render the diff:
 * the process's hunks are installed, or none are (no process applies or the
 * process withdrew) and the builtin diff runs.
 * Returns DIFF_PROCESS_EQUIVALENT when the process returns no hunks and
 * the change is not a trailing-newline-only difference; the two blobs
 * are identical, so the caller should skip diff/blame.
 * Returns DIFF_PROCESS_ERROR on process failure (already warned); the
 * caller falls back to the builtin diff.
 *
 * diff_process_clear_hunks() releases the hunks afterward.  On a
 * zero-initialized xpp it is safe even when none were installed.
 *
 * oid_a/oid_b, when non-NULL, are sent to the process as old-oid/new-oid
 * so it can key a cache on the blob pair.  Pass NULL for a side whose
 * content is not the raw blob (e.g. textconv'd) or whose object id is
 * unknown.  Any oid that is sent then names the exact bytes the process
 * receives.
 */
enum diff_process_result diff_process_fill_hunks(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		const mmfile_t *file_a,
		const mmfile_t *file_b,
		xpparam_t *xpp);

/*
 * Release the hunks diff_process_fill_hunks() installed into xpp and reset
 * the field.  Safe on a zero-initialized xpp whether or not any were
 * installed; idempotent.
 */
void diff_process_clear_hunks(xpparam_t *xpp);

/*
 * Return true when a header-only consumer may ask the diff process about
 * old_path/new_path by object id.  xdl_opts must be the flags that the
 * caller would pass to its fallback diff.  The predicate withholds the
 * consult for diff options that the protocol cannot carry, and for textconv
 * on either side because object ids name only the raw blobs.
 */
int diff_process_by_oid_eligible(struct diff_options *diffopt,
				 const char *old_path,
				 const char *new_path,
				 int xdl_opts);

/*
 * Deliver the changed line ranges for the blob pair (oid_a, oid_b) to
 * hunk_func without loading either blob.  hunk_func receives xdiff's native
 * coordinates: 0-based line starts.  Test the sign of the return (a plain
 * truthy check would hide the < 0 abort):
 *
 *   > 0  a hunks-by-oid process answered and no blob was read.  When the
 *        response carries hunks, hunk_func was called once per changed range.
 *        A no-hunks response reports the blobs equivalent, valid only when the
 *        blob line counts match; hunk_func was not called and the two blobs
 *        hold identical lines.
 *     0  no hunks-by-oid process applies (not advertised, a non-blob side,
 *        a no-hunks response with differing line counts, or a process error).
 *        A process error is warned once and drops the process for the rest of
 *        the session, unlike the per-file content path.  hunk_func was not
 *        called, so the caller loads the blobs and runs its own diff.
 *   < 0  hunk_func aborted.
 *
 * The ranges are validated against the total line counts the process returns,
 * so a misbehaving process can only mis-attribute lines, never read out of
 * range.
 *
 * Unlike diff_process_fill_hunks(), this entry point takes no xpparam_t.
 * Callers must use diff_process_by_oid_eligible() with their fallback diff
 * flags before consulting by object id.
 */
int diff_process_hunks_by_oid(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		xdl_emit_hunk_consume_func_t hunk_func,
		void *cb_data);

#endif /* DIFF_PROCESS_H */
