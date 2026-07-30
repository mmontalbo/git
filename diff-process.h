#ifndef DIFF_PROCESS_H
#define DIFF_PROCESS_H

#include "xdiff/xdiff.h"

struct diff_options;
struct object_id;
struct userdiff_driver;

/*
 * The driver whose process a consultation for path would ask, or NULL
 * when none applies (no driver, process disabled or failed, or xpp
 * carries options the tool is never told about).  Needs no content, so
 * a caller can pick a producer before loading the blobs.
 */
struct userdiff_driver *diff_process_driver(struct diff_options *diffopt,
					    const char *path,
					    const xpparam_t *xpp);

enum diff_process_result {
	DIFF_PROCESS_ERROR = -1, /* failed; caller falls back to builtin */
	DIFF_PROCESS_OK = 0,     /* hunks populated in xpp */
	DIFF_PROCESS_SKIP,       /* process did not apply: use builtin */
	DIFF_PROCESS_EQUIVALENT, /* tool says files are equivalent */
};

/*
 * Consult the diff process configured for 'path' and populate
 * xpp->external_hunks with the returned hunks.
 *
 * Handles driver lookup, flag checks (--no-ext-diff,
 * --diff-algorithm), subprocess management, and error reporting.
 *
 * Returns DIFF_PROCESS_OK when hunks are populated in xpp.
 * The caller owns xpp->external_hunks and must free() it.
 *
 * Returns DIFF_PROCESS_EQUIVALENT when the tool returns no hunks and
 * the blobs are not a trailing-newline-only change (files are
 * considered identical); caller should skip diff/blame.
 * Returns DIFF_PROCESS_SKIP when no process applies; caller
 * should use the builtin diff algorithm.
 * Returns DIFF_PROCESS_ERROR on tool failure (already warned);
 * caller should fall back to the builtin diff algorithm.
 *
 * oid_a/oid_b, when non-NULL, are sent to the tool as old-oid/new-oid
 * so it can key a cache on the blob pair.  Pass NULL for a side whose
 * content is not the raw blob (e.g. textconv'd) or whose object name is
 * unknown, so any oid that is sent always names the bytes the tool
 * receives.
 */
enum diff_process_result diff_process_fill_hunks(
		struct diff_options *diffopt,
		const char *path,
		const mmfile_t *file_a,
		const mmfile_t *file_b,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		xpparam_t *xpp);

/*
 * Ask the diff process configured for 'path' to answer from the blob
 * pair's object ids alone (the "hunks-by-oid" capability): no content
 * is loaded or sent.  On DIFF_PROCESS_OK the tool's hunks are emitted
 * through hunk_cb in 0-based emission coordinates, validated for order,
 * overlap, and lockstep alignment first; because Git holds no content,
 * the answer is used as the tool sent it, without xdiff's compaction.
 * DIFF_PROCESS_EQUIVALENT means the tool asserts the pair equal.
 * DIFF_PROCESS_SKIP covers everything that should fall through to a
 * content consult: no driver or capability, a missing object id, a
 * status=need-content answer, or an invalid response.
 */
enum diff_process_result diff_process_query_hunks(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *old_oid,
		const struct object_id *new_oid,
		const xpparam_t *xpp,
		xdl_emit_hunk_consume_func_t hunk_cb,
		void *cb_data);

#endif /* DIFF_PROCESS_H */
