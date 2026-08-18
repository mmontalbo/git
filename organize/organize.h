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
 *   [layout]   the project's directory map, hand-authored: ordered
 *     `<label>:<value> = <directory>` rules (`.` is the root). git organize
 *     matches a file's labels against the rules in order; the first rule it
 *     satisfies names the file's directory. A file matching no rule is backlog.
 *     Only a rule named in [layout] moves a file.
 *
 *   [labels]   the recorded labels, one line per source in scope, `<path> <key>=
 *     <value> ...`, with every label the project defines. A file already in its
 *     directory is listed too, so its [labels] line records its labels, apart
 *     from the directory name. A label named in no rule moves no file; git
 *     organize records it for a reader.
 *
 * The labeler and organizer commands live in config, organize.labeler and
 * organize.organizer.
 *
 *   status  Read [labels] and report the files in scope whose matching rule
 *     names a directory they are not in yet (the moves), the backlog, and a
 *     recorded path that no longer exists.
 *
 *   apply   Perform the moves. A move is a content-preserving rename. When an
 *     organizer is configured, hand it the moves; it returns edits to referring
 *     files as a patch and a reason for any move it declines. Moves and the
 *     patch apply as one git apply. With no organizer, each move is a plain git
 *     mv. A carved file's [labels] line is repointed to its new path, carrying
 *     its labels.
 *
 *   apply --labels-only  Run the labeler and write a [labels] line for every
 *     file in scope, keeping the line of a file already in its directory.
 *     Staged. This is the only path that runs a labeler.
 */

struct organize_move {
	char *src;	/* current path */
	char *dst;	/* declared path */
	char *rule_value;	/* the matched rule's value, sent to the organizer */
	char *skip_reason;	/* why the organizer declined it; NULL when it stands */
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
 * Perform the plan: consult the organizer when one is configured, then apply
 * the standing moves and the organizer's patch; the result is staged.
 * Requires a clean worktree. Records each declined move's reason in the plan.
 */
void organize_plan_apply(struct repository *repo, struct organize_plan *plan);

/*
 * Fill the [labels] record for every file in scope, staged. A file already
 * recorded keeps its line (the recorded placement is authoritative); an
 * unrecorded file is seeded from the labeler (its labels, or empty when the
 * labeler leaves it unplaced). With reseed, re-derive every line from the
 * labeler, discarding the recorded placements.
 */
void organize_run_labeler(struct repository *repo, int reseed);

void organize_plan_release(struct organize_plan *plan);

#endif /* ORGANIZE_H */
