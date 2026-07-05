#ifndef LINE_LOG_H
#define LINE_LOG_H

#include "diffcore.h" /* struct diff_filepair */
#include "range-set.h"

struct rev_info;
struct commit;
struct string_list;

/* Linked list of interesting files and their associated ranges.  The
 * list must be kept sorted by path.
 *
 * For simplicity, even though this is highly redundant, each
 * line_log_data owns its 'path'.
 */
struct line_log_data {
	struct line_log_data *next;
	char *path;
	struct range_set ranges;
	struct diff_filepair *pair;
	struct diff_ranges diff;
};

void line_log_init(struct rev_info *rev, const char *prefix, struct string_list *args);

int line_log_filter(struct rev_info *rev);
int line_log_process_ranges_arbitrary_commit(struct rev_info *rev,
						    struct commit *commit);

void line_log_queue_pairs(struct rev_info *rev, struct commit *commit);

void line_log_free(struct rev_info *rev);

#endif /* LINE_LOG_H */
