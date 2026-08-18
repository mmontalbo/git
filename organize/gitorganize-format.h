#ifndef GITORGANIZE_FORMAT_H
#define GITORGANIZE_FORMAT_H

#include "strbuf.h"
#include "string-list.h"

struct layout_rule {
	char *label;
	char *value;
	char *dir;
};

/*
 * The parsed .gitorganize file: the [scope] pathspecs, the [layout] rules, the
 * verbatim [scope] and [layout] header (round-tripped on write), and the
 * [labels] records (each path -> its "k=value ..." string in util).
 */
struct gitorganize {
	struct string_list scope;
	struct layout_rule *rules;
	size_t rules_nr, rules_alloc;
	struct strbuf header;
	struct string_list records;
};
#define GITORGANIZE_INIT { \
	.scope = STRING_LIST_INIT_DUP, \
	.header = STRBUF_INIT, \
	.records = STRING_LIST_INIT_DUP, \
}

void gitorganize_clear(struct gitorganize *g);
const char *read_path_token(const char *p, struct strbuf *out);
const char *label_value(const char *labels, const char *key, struct strbuf *out);
struct layout_rule *layout_match(struct gitorganize *g, const char *labels,
				 struct strbuf *value_buf);
void parse_gitorganize(struct gitorganize *g);
void write_gitorganize(struct gitorganize *g);

#endif /* GITORGANIZE_FORMAT_H */
