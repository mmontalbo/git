#ifndef ORGANIZE_H
#define ORGANIZE_H

#include "strbuf.h"

struct repository;
struct strvec;

/*
 * The git organize engine. It orchestrates two project tools and owns the
 * declared layout in .gitattributes:
 *
 *   labeler (organize.labeler)    context -> labels. Emits one
 *     `path \0 key \0 value \n` record per governed file: a placement is
 *     `subsystem <dir>`, a kept-at-root interface header is `role public`.
 *     It infers; it writes nothing.
 *
 *   organizer (organize.organizer)  labels -> ops. Reads the labels it is
 *     handed on stdin (same record format) and emits the op stream: a
 *     rename per file whose location differs from its label, plus the
 *     build-list reference patches those moves entail. It never touches
 *     .gitattributes.
 *
 * The builtin builds a plan (run labeler, run organizer with the selected
 * labels, materialize the declaration) and then applies it (git mv, write
 * the patches, write .gitattributes, gate on R100, stage).
 */

struct organize_label {
	char *path;
	char *key;	/* "subsystem" or "role" */
	char *value;	/* the target dir, or "public" */
};

struct organize_rename {
	char *label;
	char *src;
	char *dst;
	char *reason;	/* why a conflict is not moved; "" when ok */
	int ok;
};

struct organize_patch {
	char *path;
	char *content;
	size_t len;
};

struct organize_plan {
	struct organize_label *labels;
	size_t labels_nr, labels_alloc;
	struct organize_rename *renames;
	size_t renames_nr, renames_alloc;
	struct organize_patch *patches;
	size_t patches_nr, patches_alloc;
	struct strbuf attributes;	/* materialized .gitattributes content */
	int attributes_changed;		/* differs from the current file */
};

#define ORGANIZE_PLAN_INIT { .attributes = STRBUF_INIT }

/*
 * Run the labeler, run the organizer over the selected labels, and
 * materialize the declaration. selectors limits which labeled things are
 * organized (an entry "subsystem=<dir>" matches a label whose key=value
 * equals it); an empty selector organizes every label.
 */
void organize_plan_build(struct repository *repo,
			 const struct strvec *selectors,
			 struct organize_plan *plan);

/*
 * Perform the plan: git mv the ok renames, write the patches, write the
 * materialized .gitattributes, stage, and confirm every move is R100.
 * Requires a clean worktree; a failed step leaves the partial changes for
 * inspection.
 */
void organize_plan_apply(struct repository *repo, struct organize_plan *plan);

void organize_plan_release(struct organize_plan *plan);

#endif /* ORGANIZE_H */
