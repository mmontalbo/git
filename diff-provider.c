#include "git-compat-util.h"
#include "diff-provider-internal.h"
#include "diff-hunks.h"

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

int diff_provider_emit_hunks(const xpparam_t *xpp,
			     hunk_pair_fill_fn fill, void *fill_data,
			     xdl_emit_hunk_consume_func_t hunk_cb,
			     void *cb_data)
{
	xdemitconf_t xecfg = { .hunk_func = hunk_cb };
	xdemitcb_t ecb = { .priv = cb_data };
	mmfile_t old_file, new_file;

	if (fill(fill_data, &old_file, &new_file) < 0)
		return -1;
	return xdi_diff(&old_file, &new_file, xpp, &xecfg, &ecb);
}
