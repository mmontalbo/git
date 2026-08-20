#include "git-compat-util.h"
#include "organizer-protocol.h"
#include "organize.h"
#include "gitorganize-format.h"
#include "sequencer/apply.h"
#include "gettext.h"
#include "quote.h"
#include "run-command.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"

#define ORGANIZE_PROTOCOL "git-organize 1 organize"

/* Record a skip reason for a standing move, taking ownership of `reason`. */
static void reject_move(struct organize_plan *plan, const char *src,
			char *reason)
{
	for (size_t move_idx = 0; move_idx < plan->moves_nr; move_idx++) {
		struct organize_move *move = &plan->moves[move_idx];

		if (!strcmp(move->src, src)) {
			free(move->skip_reason);
			move->skip_reason = reason;
			return;
		}
	}
	free(reason);	/* a skip_reason for a move we did not propose is ignored */
}

/* Serialize the standing moves as the organizer request. */
static void build_request(struct organize_plan *plan, struct strbuf *request)
{
	strbuf_addstr(request, ORGANIZE_PROTOCOL "\n");
	for (size_t move_idx = 0; move_idx < plan->moves_nr; move_idx++) {
		struct organize_move *move = &plan->moves[move_idx];

		if (move->skip_reason)
			continue;
		strbuf_addstr(request, "move ");
		quote_c_style(move->src, request, NULL, 0);
		strbuf_addch(request, ' ');
		quote_c_style(move->dst, request, NULL, 0);
		strbuf_addf(request, " %s\n", move->rule_value);
	}
}

/* The next "diff --git " entry at the start of a line, or `end`. */
static const char *next_entry(const char *from, const char *end)
{
	const char *hit = memmem(from, end - from, "\ndiff --git ", 12);

	return hit ? hit + 1 : end;
}

/*
 * Validate the organizer patch before anything is applied, and record which
 * moves it claims. Each entry is resolved with git's own diff-header parser
 * (parse_git_diff_header), the same names `git apply` acts on, so a benign
 * `diff --git` line cannot disguise what the body actually does. An entry is
 * one of:
 *   - old == new: an in-place edit; the file must stay put, not be a move.
 *   - old != new: a rename-with-modification; it must match a planned move
 *     (old -> new), which git organize then assigns to the organizer (a claim).
 * A file add, delete, copy, or mode change is refused; git organize controls
 * which files exist and their modes. The patch must be in git diff format;
 * the organizer is a trusted configured command, not repository-supplied.
 */
static void validate_patch(struct organize_plan *plan,
			   const struct strbuf *patch,
			   struct string_list *claimed)
{
	struct string_list dst_map = STRING_LIST_INIT_NODUP;
	struct strbuf root = STRBUF_INIT;
	const char *cursor = patch->buf;
	const char *end = patch->buf + patch->len;

	for (size_t move_idx = 0; move_idx < plan->moves_nr; move_idx++) {
		struct organize_move *move = &plan->moves[move_idx];

		if (move->skip_reason)
			continue;
		string_list_insert(&dst_map, move->src)->util = move->dst;
	}

	while (cursor < end && *cursor == '\n')
		cursor++;
	if (cursor < end && !starts_with(cursor, "diff --git "))
		die(_("organize apply: the organizer patch is not in git diff format"));

	while (cursor < end && starts_with(cursor, "diff --git ")) {
		const char *newline = memchr(cursor, '\n', end - cursor);
		size_t first = newline ? (size_t)(newline - cursor + 1)
				       : (size_t)(end - cursor);
		struct patch entry;
		int linenr = 0, hdrlen;
		const char *old_name, *new_name;
		struct string_list_item *dst;

		memset(&entry, 0, sizeof(entry));
		hdrlen = parse_git_diff_header(&root, NULL, &linenr, 1, cursor,
					       first, end - cursor, &entry);
		if (hdrlen < 0)
			die(_("organize apply: cannot parse the organizer patch"));
		if (entry.is_new || entry.is_delete || entry.is_copy)
			die(_("organize apply: the organizer patch must not add, "
			      "delete, or copy files"));
		if (entry.old_mode && entry.new_mode &&
		    entry.old_mode != entry.new_mode)
			die(_("organize apply: the organizer patch must not change "
			      "a file's mode"));
		old_name = entry.old_name ? entry.old_name : entry.def_name;
		new_name = entry.new_name ? entry.new_name : entry.def_name;
		if (!old_name || !new_name)
			die(_("organize apply: cannot tell which file the organizer "
			      "patch touches"));

		dst = string_list_lookup(&dst_map, old_name);
		if (!strcmp(old_name, new_name)) {
			if (dst)
				die(_("organize apply: the organizer patch edits %s "
				      "in place, but it is part of a move"), old_name);
		} else if (!dst || strcmp(dst->util, new_name)) {
			die(_("organize apply: the organizer patch renames %s to %s, "
			      "which is not a planned move"), old_name, new_name);
		} else {
			string_list_insert(claimed, old_name);
		}
		release_patch(&entry);
		cursor = next_entry(cursor + hdrlen, end);
	}

	string_list_clear(&dst_map, 0);
	strbuf_release(&root);
}

/*
 * Parse the organizer response: the version line, then the reject directives
 * and the optional patch, which is validated against the plan.
 */
static void parse_response(struct organize_plan *plan,
			   const struct strbuf *response, struct strbuf *patch,
			   struct string_list *claimed)
{
	const char *cursor = response->buf;
	const char *end = response->buf + response->len;
	struct strbuf path = STRBUF_INIT;

	if (!skip_prefix(cursor, ORGANIZE_PROTOCOL, &cursor) || *cursor != '\n')
		die(_("organize: organizer did not send the protocol header '%s'"),
		    ORGANIZE_PROTOCOL);
	cursor++;

	while (cursor < end) {
		const char *newline = memchr(cursor, '\n', end - cursor);
		const char *args;

		if (!newline)
			die(_("organize: truncated organizer response"));

		if (skip_prefix(cursor, "reject ", &args)) {
			const char *reason = read_path_token(args, &path);

			while (*reason == ' ')
				reason++;
			reject_move(plan, path.buf,
				    xstrndup(reason, newline - reason));
			cursor = newline + 1;
		} else if (starts_with(cursor, "patch\n")) {
			const char *body = newline + 1;

			strbuf_add(patch, body, end - body);
			if (patch->len)
				validate_patch(plan, patch, claimed);
			break;
		} else if (newline == cursor) {
			cursor = newline + 1;	/* skip a blank line */
		} else {
			die(_("organize: unexpected organizer line"));
		}
	}
	strbuf_release(&path);
}

/* Run the organizer over the standing moves, then validate its patch. */
void run_organizer(const char *command, struct organize_plan *plan,
		   struct strbuf *patch, struct string_list *claimed)
{
	struct child_process child = CHILD_PROCESS_INIT;
	struct strbuf request = STRBUF_INIT, response = STRBUF_INIT;

	build_request(plan, &request);
	strvec_push(&child.args, command);
	child.use_shell = 1;
	if (pipe_command(&child, request.buf, request.len, &response,
			 0, NULL, 0))
		die(_("organize: organizer failed: %s"), command);
	parse_response(plan, &response, patch, claimed);
	strbuf_release(&request);
	strbuf_release(&response);
}
