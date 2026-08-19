/*
 * "git organize": reconcile a source tree against a declared layout.
 *
 * cmd_organize parses arguments and drives the organize engine (organize.c).
 * status reports the files in scope whose matching rule names a directory they
 * are not in yet (the moves), the backlog (files with no matching rule), and a
 * declared path that no longer exists. apply moves the misplaced files and
 * stages the result; apply --labels-only instead runs the labeler and records
 * the labels. --label <key[=value]> (repeatable) limits status and the move
 * apply to files carrying a matching label.
 */
#include "builtin.h"
#include "gettext.h"
#include "organize/organize.h"
#include "parse-options.h"
#include "repository.h"
#include "strvec.h"

static const char *const organize_usage[] = {
	"git organize status [--exit-code] [--label <label>]",
	"git organize apply [--label <label>]",
	"git organize apply --labels-only",
	NULL
};

static int organize_status(struct repository *repo,
			   const struct strvec *selectors, int exit_code)
{
	struct organize_plan plan = ORGANIZE_PLAN_INIT;
	int to_move, backlog, unrecorded, orphans;

	organize_plan_build(repo, selectors, &plan);
	to_move = (int)plan.moves_nr;
	backlog = (int)plan.backlog.nr;
	unrecorded = (int)plan.unrecorded.nr;
	orphans = (int)plan.orphans.nr;

	/*
	 * A summary over the files in scope: how many are already in place,
	 * how many move, and how many match no rule.
	 */
	if (plan.in_scope)
		printf(_("organize: %d in scope (%d in place, %d to move, "
			 "%d backlog)\n"),
		       plan.in_scope, plan.in_place, to_move, backlog);

	if (to_move) {
		printf(_("to move:\n"));
		for (size_t i = 0; i < plan.moves_nr; i++)
			printf("  %-32s -> %s\n", plan.moves[i].src,
			       plan.moves[i].dst);
		printf(_("%d file(s) would move\n"), to_move);
	} else {
		printf(_("in place; nothing to move\n"));
	}
	if (backlog) {
		printf(_("backlog:\n"));
		for (size_t i = 0; i < plan.backlog.nr; i++)
			printf("  %s\n", plan.backlog.items[i].string);
	}
	if (unrecorded) {
		printf(_("in scope but unrecorded:\n"));
		for (size_t i = 0; i < plan.unrecorded.nr; i++)
			printf("  %s\n", plan.unrecorded.items[i].string);
	}
	if (orphans) {
		printf(_("declared but missing:\n"));
		for (size_t i = 0; i < plan.orphans.nr; i++)
			printf("  %s\n", plan.orphans.items[i].string);
	}

	organize_plan_release(&plan);
	return exit_code && (to_move || unrecorded || orphans) ? 1 : 0;
}

static int organize_apply(struct repository *repo, const struct strvec *selectors)
{
	struct organize_plan plan = ORGANIZE_PLAN_INIT;
	int moved = 0, rejected = 0;

	organize_plan_build(repo, selectors, &plan);
	if (!plan.moves_nr) {
		printf(_("organize apply: nothing to do\n"));
		organize_plan_release(&plan);
		return 0;
	}

	organize_plan_apply(repo, &plan);

	for (size_t i = 0; i < plan.moves_nr; i++) {
		if (plan.moves[i].skip_reason)
			rejected++;
		else
			moved++;
	}
	printf(_("organize apply: %d move(s), %d skipped.\n"), moved, rejected);
	for (size_t i = 0; i < plan.moves_nr; i++)
		if (plan.moves[i].skip_reason)
			printf("  skipped %-28s %s\n", plan.moves[i].src,
			       plan.moves[i].skip_reason);
	printf(_("organize apply: the result is staged; nothing is committed.\n"));

	organize_plan_release(&plan);
	return 0;
}

int cmd_organize(int argc,
		 const char **argv,
		 const char *prefix,
		 struct repository *repo)
{
	struct strvec selectors = STRVEC_INIT;
	int exit_code = 0, labels_only = 0;
	struct option options[] = {
		OPT_STRVEC(0, "label", &selectors, N_("key[=value]"),
			   N_("limit to files carrying a matching label (repeatable)")),
		OPT_BOOL(0, "exit-code", &exit_code,
			 N_("exit non-zero from status when a file is out of place")),
		OPT_BOOL(0, "labels-only", &labels_only,
			 N_("with apply, run the labeler and record the labels")),
		OPT_END()
	};
	const char *subcmd;
	int ret;

	argc = parse_options(argc, argv, prefix, options, organize_usage, 0);
	subcmd = argc ? argv[0] : "status";
	if (argc > 1)
		die(_("git organize: too many arguments"));
	if (!strcmp(subcmd, "status")) {
		if (labels_only)
			die(_("git organize: --labels-only is an apply option"));
		ret = organize_status(repo, &selectors, exit_code);
	} else if (!strcmp(subcmd, "apply")) {
		if (labels_only) {
			organize_run_labeler(repo);
			printf(_("organize apply --labels-only: the declaration is "
				 "staged; nothing is committed.\n"));
			ret = 0;
		} else {
			ret = organize_apply(repo, &selectors);
		}
	} else {
		die(_("git organize: unknown subcommand '%s'"), subcmd);
	}

	strvec_clear(&selectors);
	return ret;
}
