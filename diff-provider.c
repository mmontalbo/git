#include "git-compat-util.h"
#include "diff-provider-internal.h"
#include "diff-hunks.h"
#include "diff-process.h"

int diff_provider_active(struct repository *r)
{
	return !!repo_diff_hunks_store(r);
}

int diff_provider_query_hunks(struct repository *r,
			      const struct object_id *old_oid,
			      const struct object_id *new_oid,
			      int xdl_opts,
			      xdl_emit_hunk_consume_func_t hunk_cb,
			      void *cb_data)
{
	if (!old_oid || !new_oid)
		return 0;
	return diff_hunks_replay(repo_diff_hunks_store(r), old_oid, new_oid,
				 xdl_opts, hunk_cb, cb_data);
}

enum diff_provider_hunks_error
diff_provider_check_hunk(struct diff_provider_hunks_check *c,
			  long old_start, long old_count,
			  long new_start, long new_count)
{
	if (old_start < 0 || old_count < 0 ||
	    new_start < 0 || new_count < 0 ||
	    old_start > INT32_MAX || old_count > INT32_MAX ||
	    new_start > INT32_MAX || new_count > INT32_MAX ||
	    (int64_t)old_start + old_count > INT32_MAX ||
	    (int64_t)new_start + new_count > INT32_MAX)
		return PROVIDER_HUNKS_RANGE;
	if (old_start < c->prev_old_end || new_start < c->prev_new_end)
		return PROVIDER_HUNKS_OVERLAP;
	if (old_start - c->prev_old_end != new_start - c->prev_new_end)
		return PROVIDER_HUNKS_MISALIGNED;
	/*
	 * With each field bounded to int32 above, the int64 sums cannot
	 * overflow even where long is 32-bit, and the range rule has
	 * already capped them at INT32_MAX.
	 */
	c->prev_old_end = (int64_t)old_start + old_count;
	c->prev_new_end = (int64_t)new_start + new_count;
	return PROVIDER_HUNKS_OK;
}

int diff_provider_emit_hunks(struct repository *r,
			     const struct object_id *old_oid,
			     const struct object_id *new_oid,
			     const char *path,
			     struct diff_options *diffopt,
			     const xpparam_t *xpp,
			     hunk_pair_fill_fn fill, void *fill_data,
			     xdl_emit_hunk_consume_func_t hunk_cb,
			     void *cb_data)
{
	xpparam_t xpp_local = *xpp;
	xdemitconf_t xecfg = { .hunk_func = hunk_cb };
	xdemitcb_t ecb = { .priv = cb_data };
	mmfile_t old_file, new_file;
	int process_failed = 0;
	int ret;

	/* Only a process consulted here may supply external hunks. */
	xpp_local.external_hunks = NULL;
	xpp_local.external_hunks_nr = 0;

	/*
	 * A process-capable driver makes the process the producer for the
	 * path, so the store (which holds xdiff's answer, one a semantic
	 * process may deliberately contradict) is not consulted.  -I
	 * patterns and anchors shape the diff but are outside the
	 * settings that key the store, so such a request is computed,
	 * never served.
	 */
	if (!diff_process_driver(diffopt, path, xpp) &&
	    !xpp->ignore_regex_nr && !xpp->anchors_nr &&
	    diff_provider_query_hunks(r, old_oid, new_oid, xpp->flags,
				      hunk_cb, cb_data))
		return 1;

	/*
	 * A process that negotiated hunks-by-oid answers the identity phase
	 * itself; only a fall-through loads content.
	 */
	switch (diff_process_query_hunks(diffopt, path, old_oid, new_oid,
					 xpp, hunk_cb, cb_data)) {
	case DIFF_PROCESS_OK:
	case DIFF_PROCESS_EQUIVALENT:
		return 0;
	case DIFF_PROCESS_ERROR:
		/*
		 * The process failed for this pair (already warned about):
		 * compute below rather than asking it again with content.
		 */
		process_failed = 1;
		break;
	default:
		break;
	}

	if (fill(fill_data, &old_file, &new_file) < 0)
		return -1;

	if (!process_failed)
		switch (diff_process_fill_hunks(diffopt, path,
						&old_file, &new_file,
						old_oid, new_oid, &xpp_local)) {
		case DIFF_PROCESS_EQUIVALENT:
			/*
			 * The process reports the pair equal: there is no hunk
			 * to emit.
			 */
			return 0;
		default:
			/* OK: process hunks now in xpp_local; else: builtin */
			break;
		}

	ret = xdi_diff(&old_file, &new_file, &xpp_local, &xecfg, &ecb) < 0 ?
		-1 : 0;
	free(xpp_local.external_hunks);
	return ret;
}
