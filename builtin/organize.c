/*
 * "git organize": reconcile a source tree against a declared layout.
 *
 * This is the command. It parses arguments, drives the organize engine
 * (organize.c), and formats the output. The engine runs two project
 * tools, the labeler (organize.labeler) and the organizer
 * (organize.organizer), and owns the declared layout in .gitattributes.
 *
 * status builds a plan and reports it: the labels and their counts, the
 * conflicts with their reasons, and whether the declaration is up to
 * date. apply builds the plan and performs it: git mv the ok renames,
 * write the reference patches and .gitattributes, gate on R100, and
 * leave the result staged for a plain git commit. --by-label
 * <key>=<value> (repeatable) limits the work to the named labels.
 */
#include "builtin.h"
#include "gettext.h"
#include "organize.h"
#include "parse-options.h"
#include "repository.h"
#include "string-list.h"
#include "strvec.h"

static const char *const organize_usage[] = {
	"git organize status [--by-label <key>=<value>]",
	"git organize apply [--by-label <key>=<value>]",
	NULL
};

/* List each conflict (a rename the organizer declined) with its reason. */
static void print_conflicts(struct organize_plan *plan)
{
	int any = 0;

	for (size_t i = 0; i < plan->renames_nr; i++) {
		if (plan->renames[i].ok)
			continue;
		if (!any++)
			printf("conflicts (not moved)\n");
		printf("  %-28s %s\n", plan->renames[i].src,
		       plan->renames[i].reason);
	}
}

/* List each label and how many ok renames it groups, sorted by label. */
static void print_label_menu(struct organize_plan *plan)
{
	struct string_list labels = STRING_LIST_INIT_DUP;

	for (size_t i = 0; i < plan->renames_nr; i++) {
		struct string_list_item *item;

		if (!plan->renames[i].ok)
			continue;
		item = string_list_insert(&labels, plan->renames[i].label);
		item->util = (void *)((intptr_t)item->util + 1);
	}
	if (labels.nr) {
		printf("labels\n");
		for (size_t i = 0; i < labels.nr; i++)
			printf("  %-28s %5d\n", labels.items[i].string,
			       (int)(intptr_t)labels.items[i].util);
	}
	string_list_clear(&labels, 0);
}

static void count_renames(struct organize_plan *plan, int *renamed,
			  int *conflict)
{
	*renamed = *conflict = 0;
	for (size_t i = 0; i < plan->renames_nr; i++) {
		if (plan->renames[i].ok)
			(*renamed)++;
		else
			(*conflict)++;
	}
}

static int organize_status(struct repository *repo,
			   const struct strvec *selectors)
{
	struct organize_plan plan = ORGANIZE_PLAN_INIT;
	int renamed, conflict;

	organize_plan_build(repo, selectors, &plan);

	if (selectors->nr) {
		int shown = 0;
		for (size_t i = 0; i < plan.renames_nr; i++)
			if (plan.renames[i].ok && shown++ < 6)
				printf("  %s -> %s\n", plan.renames[i].src,
				       plan.renames[i].dst);
		if (shown)
			printf("  ...\n");
	} else {
		print_label_menu(&plan);
	}
	print_conflicts(&plan);

	count_renames(&plan, &renamed, &conflict);
	printf("renamed    %d\n", renamed);
	printf("conflict   %d\n", conflict);
	printf("patches    %d\n", (int)plan.patches_nr);
	printf("attributes %d declared   %s\n", (int)plan.labels_nr,
	       plan.attributes_changed ? "(update pending)" : "(up to date)");

	organize_plan_release(&plan);
	return 0;
}

static int organize_apply(struct repository *repo,
			  const struct strvec *selectors)
{
	struct organize_plan plan = ORGANIZE_PLAN_INIT;
	int renamed, conflict;

	organize_plan_build(repo, selectors, &plan);
	count_renames(&plan, &renamed, &conflict);

	if (!renamed && !plan.attributes_changed) {
		printf("organize apply: nothing to do\n");
		organize_plan_release(&plan);
		return 0;
	}

	organize_plan_apply(repo, &plan);

	printf("organize apply: %d renames, %d patches, %d conflicts.\n",
	       renamed, (int)plan.patches_nr, conflict);
	print_conflicts(&plan);
	printf("organize apply: staged for review. Build to check it, then "
	       "commit with git commit, or restore with git reset --hard.\n");

	organize_plan_release(&plan);
	return 0;
}

int cmd_organize(int argc,
		 const char **argv,
		 const char *prefix,
		 struct repository *repo)
{
	struct strvec selectors = STRVEC_INIT;
	struct option options[] = {
		OPT_STRVEC(0, "by-label", &selectors, N_("key=value"),
			   N_("limit to files with this label (repeatable)")),
		OPT_END()
	};
	const char *subcmd;
	int ret;

	argc = parse_options(argc, argv, prefix, options, organize_usage, 0);
	subcmd = argc ? argv[0] : "status";
	if (!strcmp(subcmd, "status"))
		ret = organize_status(repo, &selectors);
	else if (!strcmp(subcmd, "apply"))
		ret = organize_apply(repo, &selectors);
	else
		die(_("git organize: unknown subcommand '%s'"), subcmd);

	strvec_clear(&selectors);
	return ret;
}
