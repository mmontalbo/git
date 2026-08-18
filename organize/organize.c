#include "git-compat-util.h"
#include "organize.h"
#include "gitorganize-format.h"
#include "gettext.h"
#include "pathspec.h"
#include "quote.h"
#include "read-cache-ll.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"
#include "wt-status.h"

/*
 * The [layout] rule whose directory equals `path`'s directory, or NULL when
 * `path` is a root file or its directory matches no rule. A file in a [layout]
 * directory is in place, whatever its recorded label; its directory alone
 * tells git organize it is in place.
 */
static struct layout_rule *layout_dir_rule(struct gitorganize *g,
					   const char *path)
{
	const char *slash = strrchr(path, '/');
	size_t dirlen;

	if (!slash)
		return NULL;		/* a root file */
	dirlen = slash - path;
	for (size_t i = 0; i < g->rules_nr; i++) {
		struct layout_rule *r = &g->rules[i];

		if (strcmp(r->dir, ".") && strlen(r->dir) == dirlen &&
		    !strncmp(path, r->dir, dirlen))
			return r;
	}
	return NULL;
}

/*
 * Read the index once into two lists: every tracked file in `tracked_files`,
 * and the governed subset in `scoped_files`. A file is governed when it matches
 * the [scope] pathspecs (a candidate to label and move) or when it already sits
 * in a [layout] directory (kept in scope so a file added under a carved
 * directory does not slip in ungoverned). With no [scope] pathspecs and no
 * [layout] rules, no file is governed.
 */
static void collect_index(struct repository *repo, struct gitorganize *g,
			  struct string_list *scoped_files,
			  struct string_list *tracked_files)
{
	struct pathspec pathspec;
	struct strvec specs = STRVEC_INIT;

	for (size_t i = 0; i < g->scope.nr; i++)
		strvec_push(&specs, g->scope.items[i].string);
	parse_pathspec(&pathspec, 0, PATHSPEC_PREFER_FULL, "", specs.v);

	if (repo_read_index(repo) < 0)
		die(_("organize: could not read the index"));
	for (size_t i = 0; i < repo->index->cache_nr; i++) {
		const char *name = repo->index->cache[i]->name;

		string_list_insert(tracked_files, name);
		if (g->scope.nr &&
		    match_pathspec(repo->index, &pathspec, name,
				   strlen(name), 0, NULL, 0))
			string_list_insert(scoped_files, name);
		else if (layout_dir_rule(g, name))
			string_list_insert(scoped_files, name);
	}
	clear_pathspec(&pathspec);
	strvec_clear(&specs);
}

/*
 * The parsed .gitorganize (gitorg) and the tree state that status, apply, and
 * labeling all read: the files in scope (scoped_files) and every tracked_files
 * file.
 */
struct organize_ctx {
	struct gitorganize gitorg;
	struct string_list scoped_files;
	struct string_list tracked_files;
};
#define ORGANIZE_CTX_INIT { \
	.gitorg = GITORGANIZE_INIT, \
	.scoped_files = STRING_LIST_INIT_DUP, \
	.tracked_files = STRING_LIST_INIT_DUP, \
}

static void organize_ctx_load(struct repository *repo, struct organize_ctx *ctx)
{
	gitorganize_read(&ctx->gitorg);
	collect_index(repo, &ctx->gitorg, &ctx->scoped_files,
		      &ctx->tracked_files);
}

static void organize_ctx_release(struct organize_ctx *ctx)
{
	gitorganize_clear(&ctx->gitorg);
	string_list_clear(&ctx->scoped_files, 0);
	string_list_clear(&ctx->tracked_files, 0);
}

static void add_move(struct organize_plan *plan, const char *src,
		     char *dst, const char *value)
{
	struct organize_move *m;

	ALLOC_GROW(plan->moves, plan->moves_nr + 1, plan->moves_alloc);
	m = &plan->moves[plan->moves_nr++];
	m->src = xstrdup(src);
	m->dst = dst;
	m->rule_value = xstrdup(value);
}

void organize_plan_build(struct repository *repo, struct organize_plan *plan)
{
	struct organize_ctx ctx = ORGANIZE_CTX_INIT;
	struct string_list seen = STRING_LIST_INIT_DUP;
	struct strbuf value_buf = STRBUF_INIT;

	organize_ctx_load(repo, &ctx);

	/*
	 * Classify each recorded entry by the rule its labels match. A file
	 * already in the rule's directory is in place. A file in another
	 * directory is a move. A file whose labels match no rule is backlog.
	 */
	for (size_t i = 0; i < ctx.gitorg.records.nr; i++) {
		const char *path = ctx.gitorg.records.items[i].string;
		const char *labels = ctx.gitorg.records.items[i].util;
		const char *base, *target, *slash;
		struct layout_rule *rule;
		struct strbuf dst = STRBUF_INIT;

		if (!string_list_has_string(&ctx.tracked_files, path))
			continue;	/* an orphan, handled below */
		string_list_insert(&seen, path);
		plan->in_scope++;

		rule = layout_match(&ctx.gitorg, labels, &value_buf);
		if (!rule) {
			string_list_append(&plan->backlog, path);
			continue;
		}
		target = rule->dir;

		/* the path the rule names: dir/base, or base at the root */
		slash = strrchr(path, '/');
		base = slash ? slash + 1 : path;
		if (!strcmp(target, "."))
			strbuf_addstr(&dst, base);
		else
			strbuf_addf(&dst, "%s/%s", target, base);

		if (!strcmp(dst.buf, path))
			plan->in_place++;	/* already in place */
		else
			add_move(plan, path, strbuf_detach(&dst, NULL), rule->value);
		strbuf_release(&dst);
	}

	/*
	 * The loop above covered every file with a [labels] line. A file in scope
	 * with no such line falls into one of two groups. A file already in a
	 * [layout] directory is in place; the tree is truth, so it needs no
	 * [labels] line. A root file with no line is unrecorded; it is a mismatch
	 * between the tree and [labels] and does not count toward in_scope, like
	 * an orphan.
	 */
	for (size_t i = 0; i < ctx.scoped_files.nr; i++) {
		const char *path = ctx.scoped_files.items[i].string;

		if (string_list_has_string(&seen, path))
			continue;
		if (layout_dir_rule(&ctx.gitorg, path)) {
			plan->in_scope++;
			plan->in_place++;
		} else {
			string_list_append(&plan->unrecorded, path);
		}
	}

	/* A recorded path that is no longer a tracked_files file is an orphan. */
	for (size_t i = 0; i < ctx.gitorg.records.nr; i++)
		if (!string_list_has_string(&ctx.tracked_files, ctx.gitorg.records.items[i].string))
			string_list_append(&plan->orphans, ctx.gitorg.records.items[i].string);

	organize_ctx_release(&ctx);
	string_list_clear(&seen, 0);
	strbuf_release(&value_buf);
}

/* Whether the worktree has staged or unstaged changes to any tracked file. */
static int worktree_dirty(struct repository *repo)
{
	if (repo_read_index(repo) < 0)
		die(_("organize: could not read the index"));
	return has_unstaged_changes(repo, 0) || has_uncommitted_changes(repo, 0);
}

/* Map each move's src to its dst, in `dst_of`. */
static void plan_dst_map(struct organize_plan *plan, struct string_list *dst_of)
{
	for (size_t i = 0; i < plan->moves_nr; i++)
		string_list_insert(dst_of, plan->moves[i].src)->util =
			plan->moves[i].dst;
}

/* A content-identical rename entry per move. */
static void build_rename_patch(struct organize_plan *plan, struct strbuf *out)
{
	for (size_t i = 0; i < plan->moves_nr; i++) {
		struct organize_move *m = &plan->moves[i];

		/* These are tracked_files source paths, which need no quoting. */
		strbuf_addf(out, "diff --git a/%s b/%s\n", m->src, m->dst);
		strbuf_addstr(out, "similarity index 100%\n");
		strbuf_addf(out, "rename from %s\n", m->src);
		strbuf_addf(out, "rename to %s\n", m->dst);
	}
}

static int git_apply_index(const char *patch, size_t len)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "apply", "--index", NULL);
	return pipe_command(&cp, patch, len, NULL, 0, NULL, 0);
}

/*
 * Repoint each carved file's [labels] line to its new path, carrying its labels
 * unchanged: only its location changes. Returns nonzero when [labels] changed.
 */
static int repoint_moved_declarations(struct organize_plan *plan)
{
	struct gitorganize g = GITORGANIZE_INIT;
	struct string_list dst_of = STRING_LIST_INIT_NODUP;
	struct string_list new_records = STRING_LIST_INIT_DUP;
	int changed = 0;

	plan_dst_map(plan, &dst_of);
	gitorganize_read(&g);

	/*
	 * Rewrite [labels]: an entry whose file moved (found in dst_of) takes
	 * its new path (dst_of's util) and keeps its labels (the record's util).
	 */
	for (size_t i = 0; i < g.records.nr; i++) {
		const char *path = g.records.items[i].string;
		const char *labels = g.records.items[i].util;
		struct string_list_item *moved =
			string_list_lookup(&dst_of, path);

		if (moved) {
			path = moved->util;
			changed = 1;
		}
		string_list_insert(&new_records, path)->util = xstrdup(labels);
	}

	/* Install the rewritten [labels]; gitorganize_clear frees it below. */
	string_list_clear(&g.records, 1);
	g.records = new_records;
	if (changed)
		gitorganize_write(&g);

	gitorganize_clear(&g);
	string_list_clear(&dst_of, 0);
	return changed;
}

void organize_plan_apply(struct repository *repo, struct organize_plan *plan)
{
	struct strbuf patch = STRBUF_INIT;

	if (worktree_dirty(repo))
		die(_("organize apply: the worktree has uncommitted changes; "
		      "commit or stash first"));

	/*
	 * Content-identical renames for every move, applied as one
	 * git apply --index transaction, so a failure leaves the tree untouched.
	 */
	build_rename_patch(plan, &patch);
	if (patch.len && git_apply_index(patch.buf, patch.len))
		die(_("organize apply: the change does not apply cleanly; "
		      "nothing was changed"));

	if (repoint_moved_declarations(plan)) {
		struct child_process add = CHILD_PROCESS_INIT;

		add.git_cmd = 1;
		strvec_pushl(&add.args, "add", ".gitorganize", NULL);
		if (run_command(&add))
			die(_("organize apply: staging .gitorganize failed; "
			      "restore with git reset --hard"));
	}

	strbuf_release(&patch);
}

void organize_plan_release(struct organize_plan *plan)
{
	for (size_t i = 0; i < plan->moves_nr; i++) {
		free(plan->moves[i].src);
		free(plan->moves[i].dst);
		free(plan->moves[i].rule_value);
	}
	FREE_AND_NULL(plan->moves);
	plan->moves_nr = plan->moves_alloc = 0;
	string_list_clear(&plan->backlog, 0);
	string_list_clear(&plan->unrecorded, 0);
	string_list_clear(&plan->orphans, 0);
}
