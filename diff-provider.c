#include "git-compat-util.h"
#include "diff-provider.h"

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
