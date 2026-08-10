#ifndef DIFF_PROCESS_H
#define DIFF_PROCESS_H

#include "xdiff/xdiff.h"

struct diff_options;
struct object_id;

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

#endif /* DIFF_PROCESS_H */
