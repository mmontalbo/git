/*
 * git batch: run multiple git builtin commands in a single process.
 *
 * Tracks what state each command dirtied and applies targeted resets
 * before the next command.  Falls back to spawning a child process
 * when the required reset isn't implemented yet.
 *
 * run_builtin() calls fclose(stdout) after each command, so we
 * save/restore the stdout fd with dup2() between commands.
 */
#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "read-cache-ll.h"
#include "repository.h"
#include "run-command.h"
#include "setup.h"
#include "strbuf.h"
#include "strvec.h"

#include <setjmp.h>

static sigjmp_buf batch_jmp;
static int batch_dying;

static int batch_die_is_recursing(void)
{
	return batch_dying > 1;
}

static NORETURN void batch_die_handler(const char *err, va_list params)
{
	batch_dying++;
	vfprintf(stderr, err, params);
	fprintf(stderr, "\n");
	siglongjmp(batch_jmp, 1);
}

static void restore_stdout(int saved_fd)
{
	fflush(stdout);
	if (fileno(stdout) < 0) {
		dup2(saved_fd, STDOUT_FILENO);
		stdout = fdopen(STDOUT_FILENO, "w");
	}
	close(saved_fd);
}

/*
 * Dirt tracking: after each command, mark what it may have changed.
 * Before the next command, apply targeted resets for dirty state.
 * If we can't reset something, fall back to spawning.
 */
#define DIRTY_INDEX   (1 << 0)  /* index modified (add, reset, etc.) */
#define DIRTY_CONFIG  (1 << 1)  /* config modified */
#define DIRTY_CWD     (1 << 2)  /* working directory changed */

struct cmd_class {
	const char *name;
	unsigned dirty;      /* what this command dirties */
	unsigned needs;      /* what must be clean to run in-process */
};

/*
 * Classify commands by what they dirty and what they need.
 * Commands not listed here fall back to spawning.
 */
static struct cmd_class cmd_classes[] = {
	/* Read-only: dirty nothing, need nothing special */
	{ "blame",        0,           0 },
	{ "branch",       0,           0 },
	{ "cat-file",     0,           0 },
	{ "diff",         0,           0 },
	{ "diff-files",   0,           0 },
	{ "diff-index",   0,           0 },
	{ "diff-tree",    0,           0 },
	{ "for-each-ref", 0,           0 },
	{ "log",          0,           0 },
	{ "ls-files",     0,           0 },
	{ "ls-tree",      0,           0 },
	{ "merge-base",   0,           0 },
	{ "name-rev",     0,           0 },
	{ "rev-list",     0,           0 },
	{ "rev-parse",    0,           0 },
	{ "shortlog",     0,           0 },
	{ "show",         0,           0 },
	{ "show-ref",     0,           0 },
	{ "status",       0,           0 },
	{ "symbolic-ref", 0,           0 },
	{ "tag",          0,           0 },
	{ "verify-commit",0,           0 },
	{ "verify-tag",   0,           0 },
	{ "version",      0,           0 },

	/* Index writers: dirty the index */
	{ "add",          DIRTY_INDEX, 0 },
	{ "reset",        DIRTY_INDEX, 0 },
	{ "update-index", DIRTY_INDEX, 0 },

	/* Ref + index writers */
	{ "commit",       DIRTY_INDEX, 0 },

	/* Config writers */
	{ "config",       DIRTY_CONFIG, 0 },

	{ NULL, 0, 0 },
};

static struct cmd_class *find_cmd_class(const char *name)
{
	struct cmd_class *c;
	for (c = cmd_classes; c->name; c++)
		if (!strcmp(c->name, name))
			return c;
	return NULL;
}

static void apply_resets(unsigned dirty, struct repository *repo)
{
	if (dirty & DIRTY_INDEX)
		discard_index(repo->index);
	if (dirty & DIRTY_CONFIG)
		repo_config_clear(repo);
	/* DIRTY_CWD: can't reset prefix, must spawn */
}

int cmd_batch(int argc, const char **argv, const char *prefix,
	      struct repository *repo)
{
	struct strbuf line = STRBUF_INIT;
	char *initial_cwd = xgetcwd();
	unsigned accumulated_dirt = 0;

	set_die_routine(batch_die_handler);
	set_die_is_recursing_routine(batch_die_is_recursing);

	while (1) {
		struct strvec args = STRVEC_INIT;
		struct cmd_struct *builtin;
		struct cmd_class *cls;
		const char **argv_copy;
		int ret, saved_stdout, c;
		int must_spawn = 0;

		strbuf_reset(&line);
		while ((c = fgetc(stdin)) != EOF && c != '\n') {
			strbuf_addch(&line, c);
			if (c == '\0') {
				if (line.len > 1)
					strvec_push(&args, line.buf);
				strbuf_reset(&line);
			}
		}
		if (line.len)
			strvec_push(&args, line.buf);

		if (c == EOF && !args.nr)
			break;
		if (!args.nr)
			continue;

		/* Sync CWD with the test shell */
		if (args.nr && starts_with(args.v[0], "CWD=")) {
			const char *newdir = args.v[0] + 4;
			chdir(newdir);
			if (strcmp(newdir, initial_cwd))
				accumulated_dirt |= DIRTY_CWD;
			else
				accumulated_dirt &= ~DIRTY_CWD;
			strvec_remove(&args, 0);
		}
		if (!args.nr)
			continue;

		builtin = get_builtin(args.v[0]);
		cls = find_cmd_class(args.v[0]);

		/*
		 * Decide: in-process or spawn?
		 *
		 * Spawn if:
		 * - not a builtin
		 * - not in our classified list
		 * - CWD is dirty (prefix would be wrong)
		 * - command needs state we can't reset
		 */
		if (!builtin || !cls)
			must_spawn = 1;
		else if (accumulated_dirt & DIRTY_CWD)
			must_spawn = 1;

		if (must_spawn) {
			struct child_process cp = CHILD_PROCESS_INIT;
			strvec_push(&cp.args, "git");
			strvec_pushv(&cp.args, args.v);
			ret = run_command(&cp);
			/* Child ran with fresh state, so clear our
			 * dirt flags for things the child updated */
			accumulated_dirt = 0;
		} else {
			/* Apply targeted resets for dirty state */
			apply_resets(accumulated_dirt, repo);
			accumulated_dirt = 0;

			saved_stdout = dup(STDOUT_FILENO);
			batch_dying = 0;

			DUP_ARRAY(argv_copy, args.v, args.nr + 1);

			if (sigsetjmp(batch_jmp, 1)) {
				ret = 128;
			} else {
				ret = run_builtin(builtin, args.nr,
						  argv_copy, repo);
			}

			free(argv_copy);
			restore_stdout(saved_stdout);

			/* Mark what this command dirtied */
			if (cls)
				accumulated_dirt |= cls->dirty;
		}

		fprintf(stdout, "EXIT %d\n", ret);
		fflush(stdout);

		strvec_clear(&args);
	}

	free(initial_cwd);
	strbuf_release(&line);
	return 0;
}
