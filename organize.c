#include "git-compat-util.h"
#include "organize.h"
#include "config.h"
#include "gettext.h"
#include "path.h"
#include "repository.h"
#include "run-command.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"

static const char *config_command(struct repository *repo, const char *key)
{
	const char *cmd;

	if (repo_config_get_string_tmp(repo, key, &cmd))
		die(_("set %s to the command it names"), key);
	return cmd;
}

/*
 * Run a project tool (labeler or organizer), feeding it `in` on stdin and
 * capturing its stdout into `out`. The tool runs at the worktree root.
 */
static void run_tool(struct repository *repo, const char *key,
		     const char *in, size_t in_len, struct strbuf *out)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *cmd = config_command(repo, key);

	if (!repo->worktree)
		die(_("git organize needs a worktree"));
	strvec_push(&cp.args, cmd);
	cp.use_shell = 1;
	cp.dir = repo->worktree;
	if (pipe_command(&cp, in, in_len, out, 0, NULL, 0))
		die(_("organize: %s failed: %s"), key, cmd);
}

/* Parse the labeler's `path \0 key \0 value \n` records into the plan. */
static void parse_labels(const struct strbuf *buf, struct organize_plan *plan)
{
	const char *p = buf->buf;
	const char *end = buf->buf + buf->len;

	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		const char *path, *key, *value;
		struct organize_label *l;

		if (!nl)
			die(_("organize: truncated label record"));
		path = p;
		key = path + strlen(path) + 1;
		value = key + strlen(key) + 1;
		if (value > nl)
			die(_("organize: malformed label record"));
		ALLOC_GROW(plan->labels, plan->labels_nr + 1,
			   plan->labels_alloc);
		l = &plan->labels[plan->labels_nr++];
		l->path = xstrdup(path);
		l->key = xstrdup(key);
		l->value = xmemdupz(value, nl - value);
		p = nl + 1;
	}
}

/* Parse the organizer's op stream (renames and patches) into the plan. */
static void parse_ops(const struct strbuf *buf, struct organize_plan *plan)
{
	const char *p = buf->buf;
	const char *end = buf->buf + buf->len;

	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		if (!nl)
			die(_("organize: truncated op record"));

		if (*p == 'R') {
			/* R \0 <ok> \0 <label> \0 <src> \0 <dst> \0 <reason> \n */
			const char *ok = p + 2;
			const char *label = ok + strlen(ok) + 1;
			const char *src = label + strlen(label) + 1;
			const char *dst = src + strlen(src) + 1;
			const char *reason = dst + strlen(dst) + 1;
			struct organize_rename *r;

			if (reason > nl)
				reason = nl;
			ALLOC_GROW(plan->renames, plan->renames_nr + 1,
				   plan->renames_alloc);
			r = &plan->renames[plan->renames_nr++];
			r->ok = (ok[0] == '1');
			r->label = xstrdup(label);
			r->src = xstrdup(src);
			r->dst = xstrdup(dst);
			r->reason = xmemdupz(reason, nl - reason);
			p = nl + 1;
		} else if (*p == 'P') {
			/* P \0 <path> \0 <nbytes> \n <nbytes bytes> */
			const char *path = p + 2;
			const char *nb = path + strlen(path) + 1;
			char *nbend;
			unsigned long len = strtoul(nb, &nbend, 10);
			const char *content = nl + 1;
			struct organize_patch *pt;

			if (nbend != nl)
				die(_("organize: bad patch length"));
			if (content + len > end)
				die(_("organize: truncated patch content"));
			ALLOC_GROW(plan->patches, plan->patches_nr + 1,
				   plan->patches_alloc);
			pt = &plan->patches[plan->patches_nr++];
			pt->path = xstrdup(path);
			pt->content = xmemdupz(content, len);
			pt->len = len;
			p = content + len;
		} else {
			die(_("organize: unknown op record '%c'"), *p);
		}
	}
}

/*
 * Serialize the labels the builtin selects into `out`, in the labeler's
 * record format, for the organizer's stdin. With selectors, keep only
 * labels whose key=value matches one; otherwise keep all.
 */
static void selected_labels(struct organize_plan *plan,
			    const struct strvec *selectors, struct strbuf *out)
{
	struct string_list sel = STRING_LIST_INIT_NODUP;

	for (size_t i = 0; i < selectors->nr; i++)
		string_list_append(&sel, selectors->v[i]);
	string_list_sort(&sel);

	for (size_t i = 0; i < plan->labels_nr; i++) {
		struct organize_label *l = &plan->labels[i];

		if (selectors->nr) {
			struct strbuf kv = STRBUF_INIT;
			int match;

			strbuf_addf(&kv, "%s=%s", l->key, l->value);
			match = string_list_has_string(&sel, kv.buf);
			strbuf_release(&kv);
			if (!match)
				continue;
		}
		strbuf_addstr(out, l->path);
		strbuf_addch(out, '\0');
		strbuf_addstr(out, l->key);
		strbuf_addch(out, '\0');
		strbuf_addstr(out, l->value);
		strbuf_addch(out, '\n');
	}
	string_list_clear(&sel, 0);
}

/*
 * Materialize the declaration into plan->attributes: one line per label,
 * at its post-apply path (a moved file's line follows it, repoint-and-keep),
 * git's own .gitattributes lines preserved, the organize block replaced. Set
 * attributes_changed when it differs from the current file. Idempotent.
 */
static void materialize(struct organize_plan *plan)
{
	struct string_list dst_of = STRING_LIST_INIT_NODUP; /* src -> dst */
	struct string_list decls = STRING_LIST_INIT_DUP;
	struct string_list kept = STRING_LIST_INIT_NODUP;
	struct string_list lines = STRING_LIST_INIT_NODUP;
	struct strbuf original = STRBUF_INIT, work = STRBUF_INIT;

	for (size_t i = 0; i < plan->renames_nr; i++) {
		struct organize_rename *r = &plan->renames[i];
		if (r->ok)
			string_list_insert(&dst_of, r->src)->util = r->dst;
	}

	for (size_t i = 0; i < plan->labels_nr; i++) {
		struct organize_label *l = &plan->labels[i];
		struct string_list_item *it = string_list_lookup(&dst_of, l->path);
		const char *at = it ? (const char *)it->util : l->path;
		struct strbuf line = STRBUF_INIT;

		if (!strcmp(l->key, "role"))
			strbuf_addf(&line, "/%s organize.role=%s", at, l->value);
		else
			strbuf_addf(&line, "/%s organize.subsystem=%s", at,
				    l->value);
		string_list_append(&decls, line.buf);
		strbuf_release(&line);
	}
	string_list_sort(&decls);

	if (strbuf_read_file(&original, ".gitattributes", 0) < 0)
		strbuf_reset(&original);
	strbuf_addbuf(&work, &original);
	string_list_split_in_place(&lines, work.buf, "\n", -1);
	for (size_t i = 0; i < lines.nr; i++) {
		const char *line = lines.items[i].string;
		const char *t = line;

		while (*t == ' ' || *t == '\t')
			t++;
		if (strstr(line, "organize.") ||
		    !strncmp(t, "# git-organize", strlen("# git-organize")))
			continue;
		string_list_append(&kept, line);
	}
	while (kept.nr && !*kept.items[kept.nr - 1].string)
		kept.nr--;

	strbuf_reset(&plan->attributes);
	for (size_t i = 0; i < kept.nr; i++)
		strbuf_addf(&plan->attributes, "%s\n", kept.items[i].string);
	if (plan->attributes.len)
		strbuf_addch(&plan->attributes, '\n');
	strbuf_addstr(&plan->attributes,
		      "# git-organize: declared layout (repoint-and-keep)\n");
	for (size_t i = 0; i < decls.nr; i++)
		strbuf_addf(&plan->attributes, "%s\n", decls.items[i].string);

	plan->attributes_changed =
		plan->attributes.len != original.len ||
		memcmp(plan->attributes.buf, original.buf, original.len);

	string_list_clear(&dst_of, 0);
	string_list_clear(&decls, 0);
	string_list_clear(&kept, 0);
	string_list_clear(&lines, 0);
	strbuf_release(&original);
	strbuf_release(&work);
}

void organize_plan_build(struct repository *repo,
			 const struct strvec *selectors,
			 struct organize_plan *plan)
{
	struct strbuf labels_buf = STRBUF_INIT;
	struct strbuf selected = STRBUF_INIT;
	struct strbuf ops_buf = STRBUF_INIT;

	run_tool(repo, "organize.labeler", NULL, 0, &labels_buf);
	parse_labels(&labels_buf, plan);

	selected_labels(plan, selectors, &selected);
	run_tool(repo, "organize.organizer", selected.buf, selected.len,
		 &ops_buf);
	parse_ops(&ops_buf, plan);

	materialize(plan);

	strbuf_release(&labels_buf);
	strbuf_release(&selected);
	strbuf_release(&ops_buf);
}

/* Whether the worktree has staged or unstaged tracked changes. */
static int tree_is_dirty(void)
{
	struct child_process unstaged = CHILD_PROCESS_INIT;
	struct child_process staged = CHILD_PROCESS_INIT;

	unstaged.git_cmd = 1;
	strvec_pushl(&unstaged.args, "diff", "--quiet", NULL);
	staged.git_cmd = 1;
	strvec_pushl(&staged.args, "diff", "--cached", "--quiet", NULL);
	return run_command(&unstaged) || run_command(&staged);
}

/*
 * die, leaving the partial apply on disk for inspection. The clean-tree
 * precondition makes git reset --hard the one restore step.
 */
static NORETURN void die_recover(const char *fmt, ...)
{
	va_list ap;
	struct strbuf msg = STRBUF_INIT;

	va_start(ap, fmt);
	strbuf_vaddf(&msg, fmt, ap);
	va_end(ap);
	die("%s; the changes so far are left for inspection, "
	    "restore with git reset --hard", msg.buf);
}

/*
 * Confirm git scores every ok rename R100: a content-preserving move. The
 * staged index names each rename as R<score>\t<src>\t<dst>. Build a
 * dst -> status map and check each ok rename is R100.
 */
static int all_pure_renames(struct organize_plan *plan)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	struct string_list status = STRING_LIST_INIT_DUP;
	int pure = 1;

	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "diff", "--cached", "-M", "--name-status", NULL);
	if (capture_command(&cp, &out, 0)) {
		strbuf_release(&out);
		return 0;
	}

	{
		struct string_list lines = STRING_LIST_INIT_NODUP;

		string_list_split_in_place(&lines, out.buf, "\n", -1);
		for (size_t i = 0; i < lines.nr; i++) {
			char *line = lines.items[i].string;
			char *t1 = strchr(line, '\t');
			char *dst, *t2;

			if (!t1)
				continue;
			*t1 = '\0';		/* line now holds the status */
			t2 = strchr(t1 + 1, '\t');
			dst = t2 ? t2 + 1 : t1 + 1;
			string_list_insert(&status, dst)->util = line;
		}
		string_list_clear(&lines, 0);
	}

	for (size_t i = 0; i < plan->renames_nr; i++) {
		struct string_list_item *item;

		if (!plan->renames[i].ok)
			continue;
		item = string_list_lookup(&status, plan->renames[i].dst);
		if (!item || strcmp((char *)item->util, "R100")) {
			pure = 0;
			break;
		}
	}

	string_list_clear(&status, 0);
	strbuf_release(&out);
	return pure;
}

void organize_plan_apply(struct repository *repo UNUSED,
			 struct organize_plan *plan)
{
	struct child_process add = CHILD_PROCESS_INIT;
	int moved = 0;

	if (tree_is_dirty())
		die(_("organize apply: the worktree has uncommitted changes; "
		      "commit or stash first"));

	for (size_t i = 0; i < plan->renames_nr; i++) {
		struct organize_rename *r = &plan->renames[i];
		struct child_process mv = CHILD_PROCESS_INIT;
		char *lead;

		if (!r->ok)
			continue;
		lead = xstrdup(r->dst);
		if (safe_create_leading_directories_no_share(lead) != SCLD_OK)
			die_recover(_("organize apply: could not create the "
				      "directory for %s"), r->dst);
		free(lead);
		mv.git_cmd = 1;
		strvec_pushl(&mv.args, "mv", r->src, r->dst, NULL);
		if (run_command(&mv))
			die_recover(_("organize apply: git mv %s failed"),
				    r->src);
		moved++;
	}

	for (size_t i = 0; i < plan->patches_nr; i++)
		write_file_buf(plan->patches[i].path, plan->patches[i].content,
			       plan->patches[i].len);
	if (plan->attributes_changed)
		write_file_buf(".gitattributes", plan->attributes.buf,
			       plan->attributes.len);

	add.git_cmd = 1;
	strvec_pushl(&add.args, "add", "-u", NULL);
	if (run_command(&add))
		die_recover(_("organize apply: git add -u failed"));

	if (moved && !all_pure_renames(plan))
		die_recover(_("organize apply: a move is not a pure rename "
			      "(R100)"));
}

void organize_plan_release(struct organize_plan *plan)
{
	for (size_t i = 0; i < plan->labels_nr; i++) {
		free(plan->labels[i].path);
		free(plan->labels[i].key);
		free(plan->labels[i].value);
	}
	for (size_t i = 0; i < plan->renames_nr; i++) {
		free(plan->renames[i].label);
		free(plan->renames[i].src);
		free(plan->renames[i].dst);
		free(plan->renames[i].reason);
	}
	for (size_t i = 0; i < plan->patches_nr; i++) {
		free(plan->patches[i].path);
		free(plan->patches[i].content);
	}
	FREE_AND_NULL(plan->labels);
	FREE_AND_NULL(plan->renames);
	FREE_AND_NULL(plan->patches);
	plan->labels_nr = plan->labels_alloc = 0;
	plan->renames_nr = plan->renames_alloc = 0;
	plan->patches_nr = plan->patches_alloc = 0;
	strbuf_release(&plan->attributes);
	plan->attributes_changed = 0;
}
