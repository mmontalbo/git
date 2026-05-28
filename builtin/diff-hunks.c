#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "diff.h"
#include "dir.h"
#include "diff-hunks.h"
#include "gettext.h"
#include "parse-options.h"
#include "repository.h"
#include "strbuf.h"
#include "xdiff-interface.h"

static const char * const diff_hunks_usage[] = {
	N_("git diff-hunks write --reachable"),
	N_("git diff-hunks write --path <file>"),
	N_("git diff-hunks clear"),
	NULL
};

static int get_configured_xdl_opts(struct repository *r)
{
	struct diff_options diffopts;
	int xdl_opts;

	repo_diff_setup(r, &diffopts);
	diff_setup_done(&diffopts);
	xdl_opts = diffopts.xdl_opts;
	diff_free(&diffopts);
	return xdl_opts;
}

static int cmd_diff_hunks_write(int argc, const char **argv,
				const char *prefix UNUSED,
				struct repository *r)
{
	int reachable = 0;
	const char *for_path = NULL;
	int xdl_opts;
	struct option options[] = {
		OPT_BOOL(0, "reachable", &reachable,
			 N_("process all reachable commits")),
		OPT_STRING(0, "path", &for_path, N_("file"),
			   N_("generate per-path cache for <file>")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, options, diff_hunks_usage, 0);

	if (for_path && reachable)
		die(_("options '%s' and '%s' cannot be used together"),
		    "--path", "--reachable");
	if (!for_path && !reachable)
		die(_("currently only --reachable or --path is supported"));

	xdl_opts = get_configured_xdl_opts(r);

	if (for_path)
		return write_path_hunks_file(r, for_path, xdl_opts);

	return write_diff_hunks(r, xdl_opts);
}

static int cmd_diff_hunks_clear(int argc, const char **argv,
				const char *prefix UNUSED,
				struct repository *r)
{
	struct option options[] = { OPT_END() };
	struct strbuf path = STRBUF_INIT;
	const char *objdir;

	argc = parse_options(argc, argv, NULL, options, diff_hunks_usage, 0);

	objdir = repo_get_object_directory(r);
	strbuf_addf(&path, "%s/diff-hunks", objdir);

	if (remove_dir_recursively(&path, 0) && errno != ENOENT)
		die_errno(_("unable to remove %s"), path.buf);

	strbuf_release(&path);
	return 0;
}

int cmd_diff_hunks(int argc, const char **argv, const char *prefix,
		   struct repository *repo)
{
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("write", &fn, cmd_diff_hunks_write),
		OPT_SUBCOMMAND("clear", &fn, cmd_diff_hunks_clear),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, diff_hunks_usage,
			     PARSE_OPT_SUBCOMMAND_OPTIONAL);

	if (!fn)
		usage_with_options(diff_hunks_usage, options);

	return fn(argc, argv, prefix, repo);
}
