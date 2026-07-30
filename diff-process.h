#ifndef DIFF_PROCESS_H
#define DIFF_PROCESS_H

#include "xdiff/xdiff.h"

struct diff_options;
struct object_id;
struct userdiff_driver;

/*
 * The driver whose process a consultation for path would ask, or NULL
 * when none applies (no driver, process disabled or failed, or xpp
 * carries options the process is never told about).  Needs no content, so
 * a caller can pick a producer before loading the blobs.
 */
struct userdiff_driver *diff_process_driver(struct diff_options *diffopt,
					    const char *path,
					    const xpparam_t *xpp);

enum diff_process_result {
	DIFF_PROCESS_ERROR = -1, /* failed; caller falls back to builtin */
	DIFF_PROCESS_OK = 0,     /* the process supplied hunks */
	DIFF_PROCESS_SKIP,       /* process did not apply: use builtin */
	DIFF_PROCESS_EQUIVALENT, /* process says files are equivalent */
};

/*
 * Ask the diff process configured for 'path' to answer from the blob
 * pair's object ids alone (the "hunks-by-oid" capability): no content
 * is loaded or sent.  On DIFF_PROCESS_OK the process's hunks are emitted
 * through hunk_cb in 0-based emission coordinates, validated for order,
 * overlap, and lockstep alignment first; because Git holds no content,
 * the answer is used as the process sent it, without xdiff's compaction.
 * DIFF_PROCESS_EQUIVALENT means the process asserts the pair equal.
 * DIFF_PROCESS_SKIP covers everything that should fall through to the
 * builtin computation: no driver or capability, a missing object id, a
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
