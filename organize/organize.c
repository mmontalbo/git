#include "git-compat-util.h"
#include "organize.h"
#include "gitorganize-format.h"
#include "labeler-protocol.h"
#include "organizer-protocol.h"
#include "setup/config.h"
#include "gettext.h"
#include "index/pathspec.h"
#include "quote.h"
#include "index/read-cache-ll.h"
#include "setup/repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"
#include "index/wt-status.h"

/* The configured command organize.<key> (labeler or organizer), or NULL. */
static const char *organize_command(struct repository *repo, const char *key)
{
	struct strbuf k = STRBUF_INIT;
	const char *cmd;

	strbuf_addf(&k, "organize.%s", key);
	if (repo_config_get_string_tmp(repo, k.buf, &cmd))
		cmd = NULL;
	strbuf_release(&k);
	return cmd;
}

/*
 * Whether a file's labels satisfy any --label selector. labels is the file's
 * space-separated "key=value" list. A "key=value" selector must equal a label
 * exactly; a bare "key" matches any value of that label.
 */
static int label_selected(const char *labels, struct string_list *selectors)
{
	for (size_t i = 0; i < selectors->nr; i++) {
		const char *selector = selectors->items[i].string;
		size_t sellen = strlen(selector);
		int bare = !strchr(selector, '=');

		for (const char *l = labels; *l; ) {
			const char *sp = strchrnul(l, ' ');

			if (!strncmp(l, selector, sellen) &&
			    (bare ? l[sellen] == '=' : sp == l + sellen))
				return 1;
			l = *sp ? sp + 1 : sp;
		}
	}
	return 0;
}

/*
 * The [layout] rule that places files directly in `path`'s directory, or NULL
 * when `path` is a root file or its directory names no rule. A file so placed
 * sits where the layout puts it, whatever its recorded label, so its directory
 * decides that it is in place.
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
 * in a [layout] directory (placed there, kept in scope so a file added under a
 * carved directory does not slip in ungoverned). [layout] rules decide
 * placement. With no pathspecs and no layout only the root candidates are out.
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
	parse_gitorganize(&ctx->gitorg);
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
	m->skip_reason = NULL;
}

void organize_plan_build(struct repository *repo, const struct strvec *selectors,
			 struct organize_plan *plan)
{
	struct organize_ctx ctx = ORGANIZE_CTX_INIT;
	struct string_list want = STRING_LIST_INIT_NODUP;
	struct string_list seen = STRING_LIST_INIT_DUP;
	struct strbuf value_buf = STRBUF_INIT;

	organize_ctx_load(repo, &ctx);

	for (size_t i = 0; i < selectors->nr; i++)
		string_list_append(&want, selectors->v[i]);

	/*
	 * Classify each recorded entry by the rule its labels match: a rule to
	 * the root (or one already in place) is in place; a rule to another
	 * directory is a move; no matching rule is backlog.
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

		/* the path the rule wants: dir/base, or base at the root */
		slash = strrchr(path, '/');
		base = slash ? slash + 1 : path;
		if (!strcmp(target, "."))
			strbuf_addstr(&dst, base);
		else
			strbuf_addf(&dst, "%s/%s", target, base);

		if (!strcmp(dst.buf, path))
			plan->in_place++;	/* already in place */
		else if (!want.nr || label_selected(labels, &want))
			add_move(plan, path, strbuf_detach(&dst, NULL), rule->value);
		strbuf_release(&dst);
	}

	/*
	 * A governed file the recorded census has not seen is either placed by
	 * its directory (a file sitting in a [layout] directory: in place, tree
	 * as truth, no [labels] line needed) or unrecorded (a root candidate the
	 * census has not seen, a mismatch between the tree and [labels] that does
	 * not count toward in_scope, like an orphan).
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
	string_list_clear(&want, 0);
	string_list_clear(&seen, 0);
	strbuf_release(&value_buf);
}

/* Whether the worktree has staged or unstaged changes to tracked_files files. */
static int worktree_dirty(struct repository *repo)
{
	if (repo_read_index(repo) < 0)
		die(_("organize: could not read the index"));
	return has_unstaged_changes(repo, 0) || has_uncommitted_changes(repo, 0);
}

/* Map each standing (non-rejected) move's src to its dst, in `dst_of`. */
static void plan_dst_map(struct organize_plan *plan, struct string_list *dst_of)
{
	for (size_t i = 0; i < plan->moves_nr; i++)
		if (!plan->moves[i].skip_reason)
			string_list_insert(dst_of, plan->moves[i].src)->util =
				plan->moves[i].dst;
}

/*
 * Emit `<prefix><name>` for a `diff --git` header the way git's diff does: when
 * the prefixed path needs C-quoting, wrap the whole unit in one pair of double
 * quotes ("a/<escaped>") rather than quoting the name alone. An ordinary path
 * is emitted verbatim, so the header is byte-identical to git's diff.
 */
static void add_diff_git_path(struct strbuf *out, const char *prefix,
			      const char *name)
{
	if (quote_c_style(prefix, NULL, NULL, CQUOTE_NODQ) ||
	    quote_c_style(name, NULL, NULL, CQUOTE_NODQ)) {
		strbuf_addch(out, '"');
		quote_c_style(prefix, out, NULL, CQUOTE_NODQ);
		quote_c_style(name, out, NULL, CQUOTE_NODQ);
		strbuf_addch(out, '"');
	} else {
		strbuf_addstr(out, prefix);
		strbuf_addstr(out, name);
	}
}

/*
 * A content-identical rename entry per standing move the organizer did not
 * claim. A claimed move is one the organizer renames itself, with content
 * changes, so git leaves that entry to the organizer's patch.
 */
static void build_rename_patch(struct organize_plan *plan,
			       struct string_list *claimed, struct strbuf *out)
{
	for (size_t i = 0; i < plan->moves_nr; i++) {
		struct organize_move *m = &plan->moves[i];

		if (m->skip_reason || string_list_has_string(claimed, m->src))
			continue;
		/*
		 * C-quote the paths the way git's diff does, so a byte special
		 * to the patch format (newline, quote, ...) is escaped. An
		 * ordinary path passes through unquoted, so this is byte-for-
		 * byte git's rename header.
		 */
		strbuf_addstr(out, "diff --git ");
		add_diff_git_path(out, "a/", m->src);
		strbuf_addch(out, ' ');
		add_diff_git_path(out, "b/", m->dst);
		strbuf_addch(out, '\n');
		strbuf_addstr(out, "similarity index 100%\n");
		strbuf_addstr(out, "rename from ");
		quote_c_style(m->src, out, NULL, 0);
		strbuf_addstr(out, "\nrename to ");
		quote_c_style(m->dst, out, NULL, 0);
		strbuf_addch(out, '\n');
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
 * unchanged: only its location changes. A rejected move stays put and keeps its
 * line. Returns nonzero when [labels] changed.
 */
static int repoint_moved_declarations(struct organize_plan *plan)
{
	struct gitorganize g = GITORGANIZE_INIT;
	struct string_list dst_of = STRING_LIST_INIT_NODUP;
	struct string_list new_records = STRING_LIST_INIT_DUP;
	int changed = 0;

	plan_dst_map(plan, &dst_of);
	parse_gitorganize(&g);

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
		write_gitorganize(&g);

	gitorganize_clear(&g);
	string_list_clear(&dst_of, 0);
	return changed;
}

void organize_plan_apply(struct repository *repo, struct organize_plan *plan)
{
	struct strbuf patch = STRBUF_INIT;
	struct strbuf combined = STRBUF_INIT;
	struct string_list claimed = STRING_LIST_INIT_DUP;
	const char *organizer;

	if (worktree_dirty(repo))
		die(_("organize apply: the worktree has uncommitted changes; "
		      "commit or stash first"));

	organizer = organize_command(repo, "organizer");
	if (organizer)
		run_organizer(organizer, plan, &patch, &claimed);

	/*
	 * Content-identical renames for every unclaimed move, then the
	 * organizer's edits (when any), apply as one git apply --index: git
	 * checks the whole patch before it writes, so a patch that does not
	 * apply changes nothing. The [labels] update in .gitorganize follows the
	 * apply and is staged separately rather than as part of it.
	 */
	build_rename_patch(plan, &claimed, &combined);
	if (patch.len)
		strbuf_addbuf(&combined, &patch);
	if (combined.len && git_apply_index(combined.buf, combined.len))
		die(_("organize apply: the change does not apply cleanly; "
		      "nothing was changed"));
	strbuf_release(&combined);

	if (repoint_moved_declarations(plan)) {
		struct child_process add = CHILD_PROCESS_INIT;

		add.git_cmd = 1;
		strvec_pushl(&add.args, "add", ".gitorganize", NULL);
		if (run_command(&add))
			die(_("organize apply: staging .gitorganize failed; "
			      "restore with git reset --hard"));
	}

	string_list_clear(&claimed, 0);
	strbuf_release(&patch);
}

void organize_run_labeler(struct repository *repo, int reseed)
{
	struct organize_ctx ctx = ORGANIZE_CTX_INIT;
	struct string_list labeled = STRING_LIST_INIT_DUP;
	struct string_list records = STRING_LIST_INIT_DUP;
	struct child_process add = CHILD_PROCESS_INIT;
	const char *cmd;

	organize_ctx_load(repo, &ctx);
	cmd = organize_command(repo, "labeler");
	if (!cmd)
		die(_("organize: organize.labeler is not set"));
	run_labeler(cmd, &ctx.scoped_files, &labeled);

	/*
	 * A [labels] line for every scoped_files file. A file already recorded
	 * keeps its line: the recorded placement is authoritative, so a decision
	 * made by hand or in an earlier run survives, and the labeler only seeds a
	 * file it has not yet placed. With reseed, take the labeler's fresh label
	 * for every file instead, discarding the recorded placements. Walking the
	 * scoped_files set keeps a new, unrecorded source visible either way. The
	 * loop below then preserves every placed file already listed.
	 */
	for (size_t i = 0; i < ctx.scoped_files.nr; i++) {
		const char *path = ctx.scoped_files.items[i].string;
		struct string_list_item *rec =
			string_list_lookup(&ctx.gitorg.records, path);
		struct string_list_item *it;

		if (rec && !reseed) {
			string_list_insert(&records, path)->util =
				xstrdup((const char *)rec->util);
			continue;
		}
		if (layout_dir_rule(&ctx.gitorg, path))
			continue;	/* placed by its directory; no line needed */
		it = string_list_lookup(&labeled, path);
		string_list_insert(&records, path)->util =
			xstrdup(it ? (const char *)it->util : "");
	}
	for (size_t i = 0; i < ctx.gitorg.records.nr; i++) {
		const char *path = ctx.gitorg.records.items[i].string;

		if (strchr(path, '/') &&
		    string_list_has_string(&ctx.tracked_files, path) &&
		    !string_list_has_string(&records, path))
			string_list_insert(&records, path)->util =
				xstrdup(ctx.gitorg.records.items[i].util);
	}

	string_list_clear(&ctx.gitorg.records, 1);
	ctx.gitorg.records = records;	/* ctx.gitorg owns the fresh [labels] */
	write_gitorganize(&ctx.gitorg);

	add.git_cmd = 1;
	strvec_pushl(&add.args, "add", ".gitorganize", NULL);
	if (run_command(&add))
		die(_("organize apply --labels-only: staging .gitorganize failed"));

	organize_ctx_release(&ctx);
	string_list_clear(&labeled, 1);
}

void organize_plan_release(struct organize_plan *plan)
{
	for (size_t i = 0; i < plan->moves_nr; i++) {
		free(plan->moves[i].src);
		free(plan->moves[i].dst);
		free(plan->moves[i].rule_value);
		free(plan->moves[i].skip_reason);
	}
	FREE_AND_NULL(plan->moves);
	plan->moves_nr = plan->moves_alloc = 0;
	string_list_clear(&plan->backlog, 0);
	string_list_clear(&plan->unrecorded, 0);
	string_list_clear(&plan->orphans, 0);
}
