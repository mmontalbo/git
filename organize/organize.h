#ifndef ORGANIZE_H
#define ORGANIZE_H

#include "string-list.h"

struct repository;

/*
 * The git organize engine. A project declares where each file belongs, and
 * git organize reconciles the tree against that declaration.
 *
 * The committed declaration lives in .gitorganize, a file organize owns, in
 * three sections:
 *
 *   [scope]    the scope pathspecs, one per line; no [scope] section means
 *     nothing is in scope.
 *
 *   [layout]   the project's placement map, hand-authored: ordered
 *     `<label>:<value> = <directory>` rules (`.` is the root). A file's labels
 *     are matched against the rules in order, and the first rule it satisfies
 *     places it; a file matching no rule is backlog. Only a label named in
 *     a rule places a file.
 *
 *   [labels]   the recorded labels, one line per source in scope, `<path> <key>=
 *     <value> ...`, with every label the project defines. Placed files are listed
 *     too, so a placed file's [labels] line records its labels, independently of
 *     the directory name.
 *
 *   status  Read [labels] and report the files in scope whose matching rule
 *     names a directory they are not in yet (the moves), the backlog, and a
 *     recorded path that no longer exists.
 *
 *   apply   Perform the moves. A move is a content-identical rename, applied as
 *     one git apply --index transaction. A carved file's [labels] line is
 *     repointed to its new path, carrying its labels.
 */

struct organize_move {
	char *src;	/* current path */
	char *dst;	/* declared path */
	char *rule_value;	/* the matched rule's value */
};

struct organize_plan {
	struct organize_move *moves;
	size_t moves_nr, moves_alloc;
	struct string_list backlog;	/* recorded files that match no rule */
	struct string_list unrecorded;	/* scope files with no [labels] record */
	struct string_list orphans;	/* declared paths that no longer exist */
	int in_scope;			/* files in scope */
	int in_place;			/* files in scope already at their declared location */
};

#define ORGANIZE_PLAN_INIT { \
	.backlog = STRING_LIST_INIT_DUP, \
	.unrecorded = STRING_LIST_INIT_DUP, \
	.orphans = STRING_LIST_INIT_DUP, \
}

/*
 * Read the .gitorganize declaration and record every file whose matching
 * rule names a directory it is not in as a move. Also record the backlog
 * (recorded files that match no rule), the unrecorded files (in scope,
 * no [labels] line), and the orphans (declared paths
 * that no longer exist).
 */
void organize_plan_build(struct repository *repo, struct organize_plan *plan);

/*
 * Perform the plan: apply the moves as one content-identical-rename
 * transaction; the result is staged. Requires a clean worktree.
 */
void organize_plan_apply(struct repository *repo, struct organize_plan *plan);

void organize_plan_release(struct organize_plan *plan);

#endif /* ORGANIZE_H */
