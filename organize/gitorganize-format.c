#include "git-compat-util.h"
#include "gitorganize-format.h"
#include "gettext.h"
#include "quote.h"
#include "index/read-cache-ll.h"
#include "strbuf.h"
#include "string-list.h"
#include "wrapper.h"

void gitorganize_clear(struct gitorganize *g)
{
	for (size_t i = 0; i < g->rules_nr; i++) {
		free(g->rules[i].label);
		free(g->rules[i].value);
		free(g->rules[i].dir);
	}
	FREE_AND_NULL(g->rules);
	g->rules_nr = g->rules_alloc = 0;
	strbuf_release(&g->header);
	string_list_clear(&g->scope, 0);
	string_list_clear(&g->records, 1);	/* util is an xstrdup'd string */
}

static void layout_add(struct gitorganize *g, const char *label,
		       const char *value, const char *dir)
{
	struct layout_rule *rule;

	ALLOC_GROW(g->rules, g->rules_nr + 1, g->rules_alloc);
	rule = &g->rules[g->rules_nr++];
	rule->label = xstrdup(label);
	rule->value = xstrdup(value);
	rule->dir = xstrdup(dir);
}

/* Read a path token (C-quoted or bare) at `p` into `out`; return its end. */
const char *read_path_token(const char *p, struct strbuf *out)
{
	size_t len;

	strbuf_reset(out);
	if (*p == '"') {
		const char *endp;
		if (unquote_c_style(out, p, &endp))
			die(_("organize: malformed quoted path"));
		return endp;
	}
	len = strcspn(p, " \t\n");
	strbuf_add(out, p, len);
	return p + len;
}

/*
 * The value of `key` in a space-separated "k=value ..." string, into `out`;
 * returns out->buf, or NULL when the key is absent.
 */
const char *label_value(const char *labels, const char *key,
			struct strbuf *out)
{
	size_t keylen = strlen(key);

	while (*labels) {
		const char *sp = strchrnul(labels, ' ');

		if (!strncmp(labels, key, keylen) && labels[keylen] == '=') {
			strbuf_reset(out);
			strbuf_add(out, labels + keylen + 1,
				   sp - (labels + keylen + 1));
			return out->buf;
		}
		labels = *sp ? sp + 1 : sp;
	}
	return NULL;
}

/*
 * The first rule a file's labels satisfy, or NULL (the backlog). Rules are
 * tried in order, so an earlier rule takes precedence.
 */
struct layout_rule *layout_match(struct gitorganize *g, const char *labels,
				 struct strbuf *value_buf)
{
	for (size_t i = 0; i < g->rules_nr; i++) {
		struct layout_rule *rule = &g->rules[i];
		const char *value = label_value(labels, rule->label, value_buf);

		if (value && !strcmp(value, rule->value))
			return rule;
	}
	return NULL;
}

/*
 * Parse a `label:value = directory` [layout] rule and add it to `g`; die on a
 * malformed rule or an unsafe directory. `line` is the original text, for
 * error messages.
 */
static void parse_layout_rule(struct gitorganize *g, const char *trimmed,
			      const char *line)
{
	const char *equals = strchr(trimmed, '='), *colon;
	struct strbuf left = STRBUF_INIT, dir = STRBUF_INIT;
	struct strbuf label = STRBUF_INIT, value = STRBUF_INIT;

	if (!equals)
		die(_("organize: .gitorganize: [layout] requires "
		      "'label:value = directory' in: %s"), line);
	strbuf_add(&left, trimmed, equals - trimmed);
	strbuf_trim(&left);
	strbuf_addstr(&dir, equals + 1);
	strbuf_trim(&dir);
	colon = strchr(left.buf, ':');
	if (!colon)
		die(_("organize: .gitorganize: [layout] requires "
		      "'label:value = directory' in: %s"), line);
	strbuf_add(&label, left.buf, colon - left.buf);
	strbuf_trim(&label);
	strbuf_addstr(&value, colon + 1);
	strbuf_trim(&value);
	if (!label.len || !value.len || !dir.len)
		die(_("organize: .gitorganize: [layout] requires "
		      "'label:value = directory' in: %s"), line);
	if (strcmp(dir.buf, ".") && !verify_path(dir.buf, 0))
		die(_("organize: .gitorganize: [layout] directory "
		      "must be inside the tree: %s"), dir.buf);
	layout_add(g, label.buf, value.buf, dir.buf);
	strbuf_release(&left);
	strbuf_release(&dir);
	strbuf_release(&label);
	strbuf_release(&value);
}

/*
 * Parse .gitorganize into `g`: the [scope] pathspecs (g->scope), the [layout]
 * rules (g->rules), the verbatim [scope] and [layout] header (g->header, kept
 * for round-tripping on write), and the [labels] records (g->records, each
 * path -> its "k=value k=value" string in util). A missing file leaves `g`
 * empty.
 */
void parse_gitorganize(struct gitorganize *g)
{
	struct strbuf buf = STRBUF_INIT;
	struct string_list lines = STRING_LIST_INIT_NODUP;
	struct strbuf path = STRBUF_INIT;
	enum { NONE, SCOPE, LAYOUT, LABELS } section = NONE;

	if (strbuf_read_file(&buf, ".gitorganize", 0) < 0) {
		strbuf_release(&buf);
		return;
	}
	string_list_split_in_place(&lines, buf.buf, "\n", -1);
	for (size_t i = 0; i < lines.nr; i++) {
		const char *line = lines.items[i].string;
		const char *trimmed = line;

		if (!*line && i + 1 == lines.nr)
			continue;	/* trailing empty from the final newline */
		trimmed += strspn(trimmed, " \t");

		if (!strcmp(trimmed, "[scope]")) {
			section = SCOPE;
			strbuf_addf(&g->header, "%s\n", line);
			continue;
		}
		if (!strcmp(trimmed, "[layout]")) {
			section = LAYOUT;
			strbuf_addf(&g->header, "%s\n", line);
			continue;
		}
		if (!strcmp(trimmed, "[labels]")) {
			section = LABELS;
			continue;
		}

		/*
		 * [scope] and [layout] are hand-authored and round-tripped, so
		 * every line (comments and blanks included) is kept in the
		 * header; only a content line feeds the parsed rules.
		 */
		if (section == SCOPE || section == LAYOUT) {
			strbuf_addf(&g->header, "%s\n", line);
			if (!*trimmed || *trimmed == '#')
				continue;
			if (section == SCOPE)
				string_list_append(&g->scope, trimmed);
			else
				parse_layout_rule(g, trimmed, line);
			continue;
		}

		if (section == LABELS) {
			const char *p;

			if (!*trimmed || *trimmed == '#')
				continue;
			p = read_path_token(trimmed, &path);
			if (!path.len)
				die(_("organize: .gitorganize: [labels] line has "
				      "no path: %s"), line);
			p += strspn(p, " \t");
			if (string_list_has_string(&g->records, path.buf))
				die(_("organize: .gitorganize: '%s' listed twice"),
				    path.buf);
			string_list_insert(&g->records, path.buf)->util =
				xstrdup(p);
			continue;
		}

		if (*trimmed && *trimmed != '#')
			die(_("organize: .gitorganize: line outside "
			      "[scope]/[layout]/[labels]: %s"), line);
	}
	string_list_clear(&lines, 0);
	strbuf_release(&path);
	strbuf_release(&buf);
}

/*
 * Write .gitorganize from `g`: its verbatim [scope] and [layout] header
 * (g->header), then the [labels] records (g->records, each path -> its
 * "k=value k=value" string in util), one `<path> <k=value ...>` line per entry
 * in the list's sorted order. A path that needs it is C-quoted.
 */
void write_gitorganize(struct gitorganize *g)
{
	struct strbuf out = STRBUF_INIT;

	if (g->header.len)
		strbuf_addbuf(&out, &g->header);
	else
		strbuf_addstr(&out, "[layout]\n");
	strbuf_addstr(&out, "[labels]\n");
	strbuf_addstr(&out,
		      "# git organize apply --labels-only regenerates the lines below.\n");
	for (size_t i = 0; i < g->records.nr; i++) {
		const char *labels = g->records.items[i].util;

		quote_c_style(g->records.items[i].string, &out, NULL, 0);
		if (labels && *labels) {
			strbuf_addch(&out, ' ');
			strbuf_addstr(&out, labels);
		}
		strbuf_addch(&out, '\n');
	}
	write_file_buf(".gitorganize", out.buf, out.len);
	strbuf_release(&out);
}
