/*
 * git batch: run git builtins in a single persistent process,
 * avoiding per-command process spawn overhead.
 *
 * In --all mode every builtin is accepted.  Without --all only
 * whitelisted read-only commands run in-process; anything else
 * gets EXIT -1, telling the shell wrapper to fall back to a
 * normal spawn.
 *
 * Protocol (stdin, NUL-delimited, newline-terminated):
 *   ENV=KEY=VALUE\0          (optional, applied via setenv)
 *   STDERR=<path>\0          (optional, redirect stderr)
 *   STDIN=<path>\0           (optional, redirect stdin from file)
 *   CWD=<dir>\0              (optional, chdir before command)
 *   <arg0>\0<arg1>\0...\n    (command + arguments)
 *
 * Protocol (stdout):
 *   <command output>
 *   EXIT <code>\n        (0+ = ran in-process)
 *   EXIT -1\n            (not whitelisted, caller should spawn)
 */
#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "config.h"
#include "read-cache-ll.h"
#include "refs.h"
#include "repository.h"
#include "setup.h"
#include "strbuf.h"
#include "odb.h"
#include "strvec.h"
#include "chdir-notify.h"
#include "convert.h"
#include "simple-ipc.h"
#include "pkt-line.h"
#include "trace.h"

#include <setjmp.h>

static struct trace_key trace_batch = TRACE_KEY_INIT(BATCH);

/*
 * Snapshot/restore of mutable process state between commands.
 *
 * Three categories of state leak between commands in a persistent
 * process:
 *   1. Static variables (.data + .bss segments)
 *   2. Environment variables (heap, modified by setenv/putenv)
 *   3. CWD (modified by chdir)
 *
 * We snapshot all three at startup and restore before each command.
 * Heap allocations referenced by statics leak on restore but this
 * is acceptable for a test runner.
 *
 * On Linux, the linker provides __data_start, __bss_start, _end.
 * On Windows, we'd parse the PE section headers instead.
 *
 * The snapshot struct lives on cmd_batch's stack frame so it
 * survives the .data/.bss restore.
 */
extern char **environ;

struct process_snapshot {
	/* .data segment */
	char *data_copy;
	char *data_start;
	size_t data_size;
	/* .bss segment */
	char *bss_copy;
	char *bss_start;
	size_t bss_size;
	/* environment */
	char **env_copy;
	int env_count;
};

#ifdef GIT_WINDOWS_NATIVE
#include <windows.h>

/*
 * On Windows, find .data and .bss sections via PE headers.
 */
static void find_pe_sections(char **data_start, size_t *data_size,
			     char **bss_start, size_t *bss_size)
{
	HMODULE mod = GetModuleHandleA(NULL);
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	int i;

	*data_start = *bss_start = NULL;
	*data_size = *bss_size = 0;

	for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		if (!memcmp(sec->Name, ".data", 5)) {
			*data_start = (char *)mod + sec->VirtualAddress;
			*data_size = sec->Misc.VirtualSize;
		} else if (!memcmp(sec->Name, ".bss", 4)) {
			*bss_start = (char *)mod + sec->VirtualAddress;
			*bss_size = sec->Misc.VirtualSize;
		}
	}
	if (!*data_start)
		die("batch: cannot find .data section in PE image");
}
#else
extern char __data_start[], __bss_start[], _end[];
#endif

static void snapshot_process_state(struct process_snapshot *snap)
{
	int i;

	/* Segments */
#ifdef GIT_WINDOWS_NATIVE
	find_pe_sections(&snap->data_start, &snap->data_size,
			 &snap->bss_start, &snap->bss_size);
#else
	snap->data_start = __data_start;
	snap->data_size = __bss_start - __data_start;
	snap->bss_start = __bss_start;
	snap->bss_size = _end - __bss_start;
#endif
	snap->data_copy = xmalloc(snap->data_size);
	memcpy(snap->data_copy, snap->data_start, snap->data_size);
	if (snap->bss_size) {
		snap->bss_copy = xmalloc(snap->bss_size);
		memcpy(snap->bss_copy, snap->bss_start, snap->bss_size);
	}

	/* Environment */
	for (snap->env_count = 0; environ[snap->env_count]; snap->env_count++)
		;
	ALLOC_ARRAY(snap->env_copy, snap->env_count + 1);
	for (i = 0; i < snap->env_count; i++)
		snap->env_copy[i] = xstrdup(environ[i]);
	snap->env_copy[snap->env_count] = NULL;
}

static void restore_process_state(const struct process_snapshot *snap)
{
	int i;

	/* Segments: restore .data and .bss from snapshot */
	memcpy(snap->data_start, snap->data_copy, snap->data_size);
	if (snap->bss_size)
		memcpy(snap->bss_start, snap->bss_copy, snap->bss_size);

	/* Environment: restore to initial state */
#ifdef GIT_WINDOWS_NATIVE
	/*
	 * On MinGW, getenv/putenv/unsetenv are remapped to
	 * mingw_getenv/mingw_putenv which use the Win32 API
	 * (GetEnvironmentVariableW / SetEnvironmentVariableW).
	 * The CRT 'environ' array is never touched by these.
	 * Clear the Win32 environment block directly.
	 */
	{
		wchar_t *env_block = GetEnvironmentStringsW();
		if (env_block) {
			const wchar_t *p = env_block;
			while (*p) {
				const wchar_t *eq = wcschr(p, L'=');
				if (eq && eq != p) {
					size_t key_len = eq - p;
					wchar_t *key = calloc(key_len + 1,
							      sizeof(wchar_t));
					wmemcpy(key, p, key_len);
					SetEnvironmentVariableW(key, NULL);
					free(key);
				}
				p += wcslen(p) + 1;
			}
			FreeEnvironmentStringsW(env_block);
		}
	}
#else
	clearenv();
#endif
	for (i = 0; i < snap->env_count; i++)
		putenv(xstrdup(snap->env_copy[i]));
}

/*
 * Only commands that are ALWAYS read-only regardless of arguments.
 * Commands like branch, tag, symbolic-ref are excluded because
 * they can write depending on arguments (e.g. "git branch foo").
 * hash-object is excluded because -w writes objects.
 */
static const char *readonly_cmds[] = {
	"blame", "cat-file", "diff", "diff-files", "diff-index",
	"diff-tree", "for-each-ref", "log", "ls-files", "ls-tree",
	"merge-base", "name-rev", "rev-list", "rev-parse",
	"shortlog", "show", "show-ref", "status",
	"verify-commit", "verify-tag", "version", NULL
};

static int is_readonly(const char *cmd)
{
	const char **p;
	for (p = readonly_cmds; *p; p++)
		if (!strcmp(*p, cmd))
			return 1;
	return 0;
}

#ifdef GIT_WINDOWS_NATIVE
static jmp_buf batch_jmp;
#else
static sigjmp_buf batch_jmp;
#endif
static int batch_exit_code;
static int batch_dying;

static int batch_die_is_recursing(void)
{
	return batch_dying > 1;
}

/*
 * Persistent mode: catch die() with longjmp so the batch process
 * survives and can serve the next command.
 */
static NORETURN void batch_die_handler(const char *err, va_list params)
{
	batch_dying++;
	fprintf(stderr, "fatal: ");
	vfprintf(stderr, err, params);
	fprintf(stderr, "\n");
	batch_exit_code = 128;
#ifdef GIT_WINDOWS_NATIVE
	longjmp(batch_jmp, 1);
#else
	siglongjmp(batch_jmp, 1);
#endif
}

static void restore_stdout_fd(int saved_fd)
{
	dup2(saved_fd, STDOUT_FILENO);
#ifndef GIT_WINDOWS_NATIVE
	stdout = fdopen(STDOUT_FILENO, "w");
#endif
}

/*
 * Single-shot mode: print the error and send EXIT before dying.
 * Unlike the persistent handler, this lets exit() proceed normally
 * so atexit handlers run (cleaning up temp files, etc.).
 */
static int single_shot_stdout_fd = -1;

static NORETURN void single_die_handler(const char *err, va_list params)
{
	batch_dying++;
	fprintf(stderr, "fatal: ");
	vfprintf(stderr, err, params);
	fprintf(stderr, "\n");

	if (single_shot_stdout_fd >= 0) {
		restore_stdout_fd(single_shot_stdout_fd);
		fprintf(stdout, "EXIT 128\n");
		fflush(stdout);
	}

	exit(128);
}

/*
 * Persistent mode: catch exit() calls that bypass die().
 */
static NORETURN void batch_exit_intercept(int code)
{
	batch_exit_code = code;
#ifdef GIT_WINDOWS_NATIVE
	longjmp(batch_jmp, 1);
#else
	siglongjmp(batch_jmp, 1);
#endif
}

/*
 * Single-shot mode: send EXIT on the protocol and then exit for real,
 * so atexit handlers still run.
 */
static NORETURN void single_exit_intercept(int code)
{
	if (single_shot_stdout_fd >= 0) {
		restore_stdout_fd(single_shot_stdout_fd);
		fprintf(stdout, "EXIT %d\n", (code == 129) ? -1 : code);
		fflush(stdout);
	}
	set_exit_intercept(NULL);
	exit(code);
}

static int read_command(struct strvec *args, struct strbuf *errpath,
			struct strbuf *stdinpath, struct strbuf *cwdpath,
			struct strvec *env_vars)
{
	struct strbuf tok = STRBUF_INIT;
	unsigned char ch;
	ssize_t r;

	strvec_clear(args);
	strbuf_reset(errpath);
	strbuf_reset(stdinpath);
	strbuf_reset(cwdpath);
	strvec_clear(env_vars);

	/*
	 * Use raw read() instead of fgetc(stdin) to avoid stdio
	 * buffering.  Commands that read stdin (e.g. update-index
	 * --index-info) get their stdin via dup2() on STDIN_FILENO.
	 * If fgetc() buffered ahead, the dup2'd fd would miss the
	 * data already consumed into the FILE* buffer, and the
	 * command would read stale protocol bytes instead of its
	 * intended input.
	 */
	while ((r = read(STDIN_FILENO, &ch, 1)) == 1 && ch != '\n') {
		strbuf_addch(&tok, ch);
		if (ch == '\0') {
			if (tok.len > 1) {
				if (starts_with(tok.buf, "STDERR="))
					strbuf_addstr(errpath, tok.buf + 7);
				else if (starts_with(tok.buf, "STDIN="))
					strbuf_addstr(stdinpath, tok.buf + 6);
				else if (starts_with(tok.buf, "CWD="))
					strbuf_addstr(cwdpath, tok.buf + 4);
				else if (starts_with(tok.buf, "ENV="))
					strvec_push(env_vars, tok.buf + 4);
				else
					strvec_push(args, tok.buf);
			}
			strbuf_reset(&tok);
		}
	}
	if (tok.len)
		strvec_push(args, tok.buf);

	strbuf_release(&tok);
	return (r <= 0 && !args->nr) ? -1 : 0;
}

/*
 * Parse a command from a buffer (for IPC mode).  Same protocol as
 * read_command() but from an in-memory buffer instead of stdin.
 */
static int parse_command_from_buf(const char *buf, size_t len,
				  struct strvec *args, struct strbuf *errpath,
				  struct strbuf *stdinpath, struct strbuf *cwdpath,
				  struct strvec *env_vars)
{
	struct strbuf tok = STRBUF_INIT;
	size_t i;

	strvec_clear(args);
	strbuf_reset(errpath);
	strbuf_reset(stdinpath);
	strbuf_reset(cwdpath);
	strvec_clear(env_vars);

	for (i = 0; i < len; i++) {
		char ch = buf[i];
		if (ch == '\n')
			break;
		strbuf_addch(&tok, ch);
		if (ch == '\0') {
			if (tok.len > 1) {
				if (starts_with(tok.buf, "STDERR="))
					strbuf_addstr(errpath, tok.buf + 7);
				else if (starts_with(tok.buf, "STDIN="))
					strbuf_addstr(stdinpath, tok.buf + 6);
				else if (starts_with(tok.buf, "CWD="))
					strbuf_addstr(cwdpath, tok.buf + 4);
				else if (starts_with(tok.buf, "ENV="))
					strvec_push(env_vars, tok.buf + 4);
				else
					strvec_push(args, tok.buf);
			}
			strbuf_reset(&tok);
		}
	}
	if (tok.len)
		strvec_push(args, tok.buf);

	strbuf_release(&tok);
	return args->nr ? 0 : -1;
}

/*
 * Apply environment variables from the protocol and track them
 * so they can be cleared before the next command.
 */
static void apply_env(struct strvec *env_vars, struct strvec *prev_env_keys)
{
	size_t i;

	/* Unset vars from the previous command that aren't in this one */
	for (i = 0; i < prev_env_keys->nr; i++)
		unsetenv(prev_env_keys->v[i]);
	strvec_clear(prev_env_keys);

	/* Apply this command's vars */
	for (i = 0; i < env_vars->nr; i++) {
		const char *eq = strchr(env_vars->v[i], '=');
		if (eq) {
			struct strbuf key = STRBUF_INIT;
			strbuf_add(&key, env_vars->v[i],
				   eq - env_vars->v[i]);
			setenv(key.buf, eq + 1, 1);
			strvec_push(prev_env_keys, key.buf);
			strbuf_release(&key);
		}
	}
}

/*
 * Clear the repository state so the next command's
 * setup_git_directory() rediscovers the repo from its CWD.
 */
static void batch_clear_repo(struct repository *r)
{
	/*
	 * On Windows, a process's CWD holds a handle on the directory,
	 * preventing deletion. Move to the root so test cleanup can
	 * remove the previous command's working directory.
	 */
	if (chdir("/"))
		; /* best-effort, failure is harmless */

	trace_printf_key(&trace_batch,
		"clear: gitdir=[%s] worktree=[%s] index=%s refs=%s odb=%s",
		r->gitdir ? r->gitdir : "(null)",
		r->worktree ? r->worktree : "(null)",
		r->index ? "yes" : "no",
		r->refs_private ? "yes" : "no",
		r->objects ? "yes" : "no");

	discard_index(r->index);
	if (r->refs_private) {
		ref_store_release(r->refs_private);
		FREE_AND_NULL(r->refs_private);
	}
	repo_config_clear(r);
	FREE_AND_NULL(r->worktree);
	r->worktree_initialized = 0;
	FREE_AND_NULL(r->gitdir);
	FREE_AND_NULL(r->commondir);
	if (r->objects) {
		odb_free(r->objects);
		r->objects = NULL;
	}
	chdir_notify_clear();
	reset_parsed_attributes();
}

/*
 * IPC mode: handle one command per client connection.
 * The request is the same NUL-delimited protocol as stdin mode.
 * The response is stdout output + "EXIT <code>\n", sent via reply_cb.
 */
#ifdef SUPPORTS_SIMPLE_IPC
struct batch_ipc_data {
	struct process_snapshot *snap;
	int accept_all;
};

static int batch_ipc_handler(void *data, const char *request,
			     size_t request_len,
			     ipc_server_reply_cb *reply_cb,
			     struct ipc_server_reply_data *reply_data)
{
	struct batch_ipc_data *d = data;
	struct strvec args = STRVEC_INIT;
	struct strbuf errpath = STRBUF_INIT;
	struct strbuf stdinpath = STRBUF_INIT;
	struct strbuf cwdpath = STRBUF_INIT;
	struct strvec env_vars = STRVEC_INIT;
	struct strvec prev_env_keys = STRVEC_INIT;
	struct cmd_struct *builtin;
	const char **argv_copy;
	int ret, saved_stdout = -1, saved_err = -1, saved_in = -1;
	struct strbuf output = STRBUF_INIT;
	char exit_buf[32];
	int tmpfd;
	char tmpfile_path[PATH_MAX];
	extern const char *tmp_original_cwd;

	/* Handle control commands (before any state modification) */
	if (request_len == 5 && !memcmp(request, "quit\n", 5)) {
		set_die_routine(NULL);
		set_exit_intercept(NULL);
		return SIMPLE_IPC_QUIT;
	}
	if (request_len == 5 && !memcmp(request, "ping\n", 5)) {
		reply_cb(reply_data, "pong", 4);
		return 0;
	}

	/* Parse protocol from buffer */
	if (parse_command_from_buf(request, request_len,
				   &args, &errpath, &stdinpath, &cwdpath,
				   &env_vars) < 0 || !args.nr)
		goto done;

	/* Restore process state */
	restore_process_state(d->snap);
	tmp_original_cwd = NULL;
	if (chdir("/"))
		; /* best-effort */
	set_die_routine(batch_die_handler);
	set_die_is_recursing_routine(batch_die_is_recursing);
	set_exit_intercept(NULL);
	run_builtin_keep_stdout = 1;

	apply_env(&env_vars, &prev_env_keys);
	if (cwdpath.len && chdir(cwdpath.buf))
		warning("batch: chdir(%s) failed", cwdpath.buf);

	builtin = get_builtin(args.v[0]);
	if (!builtin || (!d->accept_all && !is_readonly(args.v[0]))) {
		snprintf(exit_buf, sizeof(exit_buf), "EXIT -1\n");
		reply_cb(reply_data, exit_buf, strlen(exit_buf));
		goto done;
	}

	/* Redirect stderr to file */
	if (errpath.len) {
		int fd = open(errpath.buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			saved_err = dup(STDERR_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}

	/* Redirect stdin from file */
	if (stdinpath.len) {
		int fd = open(stdinpath.buf, O_RDONLY);
		if (fd >= 0) {
			saved_in = dup(STDIN_FILENO);
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
	}

	/* Capture stdout to a tmpfile */
	{
		const char *tmp = getenv("TMPDIR");
		if (!tmp) tmp = "/tmp";
		snprintf(tmpfile_path, sizeof(tmpfile_path),
			 "%s/batch-stdout-XXXXXX", tmp);
	}
	tmpfd = xmkstemp(tmpfile_path);
	saved_stdout = dup(STDOUT_FILENO);
	dup2(tmpfd, STDOUT_FILENO);
	close(tmpfd);

	/* Run the builtin */
	DUP_ARRAY(argv_copy, args.v, args.nr + 1);
	batch_dying = 0;
#ifdef GIT_WINDOWS_NATIVE
	if (setjmp(batch_jmp)) {
#else
	if (sigsetjmp(batch_jmp, 1)) {
#endif
		ret = batch_exit_code;
	} else {
		set_exit_intercept(batch_exit_intercept);
		ret = run_builtin(builtin, args.nr, argv_copy, the_repository);
		set_exit_intercept(NULL);
	}
	free(argv_copy);

	/* Flush and capture stdout */
	fflush(stdout);
	dup2(saved_stdout, STDOUT_FILENO);
	close(saved_stdout);
	saved_stdout = -1;

	{
		int rfd = open(tmpfile_path, O_RDONLY);
		if (rfd >= 0) {
			strbuf_read(&output, rfd, 0);
			close(rfd);
		}
	}
	unlink(tmpfile_path);

	/* Restore stderr */
	if (saved_err >= 0) {
		fflush(stderr);
		dup2(saved_err, STDERR_FILENO);
		close(saved_err);
	}

	/* Restore stdin */
	if (saved_in >= 0) {
		dup2(saved_in, STDIN_FILENO);
		close(saved_in);
	}

	if (ret == 129)
		ret = -1;
	if (chdir("/"))
		; /* best-effort */

	/* Send response: output + EXIT line */
	if (output.len)
		reply_cb(reply_data, output.buf, output.len);
	snprintf(exit_buf, sizeof(exit_buf), "EXIT %d\n", ret);
	reply_cb(reply_data, exit_buf, strlen(exit_buf));

done:
	/* Reset handlers so IPC server shutdown doesn't hit stale longjmp */
	set_die_routine(NULL);
	set_exit_intercept(NULL);

	strvec_clear(&args);
	strvec_clear(&env_vars);
	strvec_clear(&prev_env_keys);
	strbuf_release(&errpath);
	strbuf_release(&stdinpath);
	strbuf_release(&cwdpath);
	strbuf_release(&output);
	return 0;
}
#endif /* SUPPORTS_SIMPLE_IPC */

int cmd_batch(int argc, const char **argv, const char *prefix,
	      struct repository *repo)
{
	struct strvec args = STRVEC_INIT;
	struct strbuf errpath = STRBUF_INIT;
	struct strbuf stdinpath = STRBUF_INIT;
	struct strbuf cwdpath = STRBUF_INIT;
	struct strvec env_vars = STRVEC_INIT;
	struct strvec prev_env_keys = STRVEC_INIT;
	int accept_all = 0, single_shot = 0;
	const char *ipc_path = NULL;
	struct process_snapshot snap;

	/*
	 * With flags=0 in the command table, run_builtin() passes
	 * NULL because no repo setup ran.  We need the_repository
	 * for clearing state and passing to sub-commands.
	 */
	if (!repo)
		repo = the_repository;

	for (int i = 1; i < argc; i++) {
		if (skip_prefix(argv[i], "--ipc=", &ipc_path))
			accept_all = 1;
		else if (!strcmp(argv[i], "--all"))
			accept_all = 1;
		else if (!strcmp(argv[i], "--single"))
			single_shot = 1;
	}

	/*
	 * Release inherited CWD immediately so test cleanup can
	 * remove the trash directory while idle workers wait.
	 */
	if (chdir("/"))
		; /* best-effort */

	/*
	 * Snapshot the .data and .bss segments so we can reset all
	 * static variables between commands.  This eliminates stale
	 * state from parse_options targets, config-loaded globals,
	 * and any other module-level singletons.
	 */
	if (!single_shot)
		snapshot_process_state(&snap);

#ifdef SUPPORTS_SIMPLE_IPC
	if (ipc_path) {
		struct ipc_server_opts ipc_opts = { .nr_threads = 1 };
		struct batch_ipc_data ipc_data = {
			.snap = &snap,
			.accept_all = accept_all,
		};
		return ipc_server_run(ipc_path, &ipc_opts,
				      batch_ipc_handler, &ipc_data);
	}
#endif

	/*
	 * In persistent mode, catch die()/exit() via longjmp so the
	 * process survives.  In single-shot mode (pool workers), use
	 * a die handler that sends EXIT before calling real exit(),
	 * so atexit handlers run and clean up temp files.
	 */
	if (single_shot) {
		set_die_routine(single_die_handler);
	} else {
		set_die_routine(batch_die_handler);
	}
	set_die_is_recursing_routine(batch_die_is_recursing);
	run_builtin_keep_stdout = 1;

	while (read_command(&args, &errpath, &stdinpath, &cwdpath,
			    &env_vars) == 0) {
		struct cmd_struct *builtin;
		const char **argv_copy;
		int ret, saved_stdout, saved_err = -1, saved_in = -1;

		if (!single_shot) {
			extern const char *tmp_original_cwd;

			restore_process_state(&snap);
			/*
			 * tmp_original_cwd is set once during startup and
			 * consumed (set to NULL) by setup_original_cwd().
			 * The .data restore brings it back, causing
			 * strbuf_realpath() to re-resolve on every command.
			 * On Windows, this eventually blocks. Null it out
			 * since the original CWD was already processed.
			 */
			tmp_original_cwd = NULL;
			if (chdir("/"))
				; /* best-effort */
			set_die_routine(batch_die_handler);
			set_die_is_recursing_routine(batch_die_is_recursing);
			set_exit_intercept(NULL);
			run_builtin_keep_stdout = 1;
		} else {
			batch_clear_repo(repo);
		}

		if (!args.nr)
			continue;

		if (trace_want(&trace_batch)) {
			struct strbuf cmd = STRBUF_INIT;
			for (size_t i = 0; i < args.nr; i++) {
				if (i) strbuf_addch(&cmd, ' ');
				strbuf_addstr(&cmd, args.v[i]);
			}
			trace_printf_key(&trace_batch,
				"recv: cmd=[%s] cwd=[%s] stdin=[%s] stderr=[%s] env_nr=%d",
				cmd.buf, cwdpath.buf, stdinpath.buf,
				errpath.buf, (int)env_vars.nr);
			strbuf_release(&cmd);
		}

		apply_env(&env_vars, &prev_env_keys);

		if (cwdpath.len && chdir(cwdpath.buf))
			warning("batch: chdir(%s) failed", cwdpath.buf);

		builtin = get_builtin(args.v[0]);
		if (!builtin || (!accept_all && !is_readonly(args.v[0]))) {
			trace_printf_key(&trace_batch, "reject: %s", args.v[0]);
			fprintf(stdout, "EXIT -1\n");
			fflush(stdout);
			continue;
		}

		/* Redirect stderr to temp file */
		if (errpath.len) {
			int fd = open(errpath.buf,
				      O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0) {
				saved_err = dup(STDERR_FILENO);
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
		}

		/* Redirect stdin from file (for commands that read stdin) */
		if (stdinpath.len) {
			int fd = open(stdinpath.buf, O_RDONLY);
			if (fd >= 0) {
				saved_in = dup(STDIN_FILENO);
				dup2(fd, STDIN_FILENO);
				close(fd);
				trace_printf_key(&trace_batch,
					"stdin: redirected from %s", stdinpath.buf);
			} else {
				trace_printf_key(&trace_batch,
					"stdin: FAILED to open %s (errno=%d)",
					stdinpath.buf, errno);
			}
		}

		/* Save stdout (run_builtin may fclose it) */
		saved_stdout = dup(STDOUT_FILENO);
		if (single_shot)
			single_shot_stdout_fd = saved_stdout;

		DUP_ARRAY(argv_copy, args.v, args.nr + 1);

		if (single_shot) {
			set_exit_intercept(single_exit_intercept);
			ret = run_builtin(builtin, args.nr, argv_copy, repo);
			set_exit_intercept(NULL);
		} else {
			batch_dying = 0;
#ifdef GIT_WINDOWS_NATIVE
			if (setjmp(batch_jmp)) {
#else
			if (sigsetjmp(batch_jmp, 1)) {
#endif
				ret = batch_exit_code;
			} else {
				set_exit_intercept(batch_exit_intercept);
				ret = run_builtin(builtin, args.nr, argv_copy, repo);
				set_exit_intercept(NULL);
			}
		}

		if (single_shot)
			single_shot_stdout_fd = -1;

		free(argv_copy);

		fflush(stdout);

		/* Restore stdout fd */
		restore_stdout_fd(saved_stdout);
		close(saved_stdout);

		/* Restore stderr */
		if (saved_err >= 0) {
			fflush(stderr);
			dup2(saved_err, STDERR_FILENO);
			close(saved_err);
		}

		/* Restore stdin */
		if (saved_in >= 0) {
			dup2(saved_in, STDIN_FILENO);
			close(saved_in);
		}

		/*
		 * Exit code 129 is a usage error from parse_options.
		 * Previously this killed the worker and the shell
		 * retried via direct spawn.  Send EXIT -1 to preserve
		 * that fallback behavior.
		 */
		if (ret == 129)
			ret = -1;

		/*
		 * Release the CWD before responding, so the shell's
		 * test_when_finished cleanup can delete the directory.
		 * On Windows, a process's CWD holds the dir open.
		 */
		if (chdir("/"))
			; /* best-effort */

		trace_printf_key(&trace_batch, "done: %s -> EXIT %d",
			args.v[0], ret);
		/*
		 * Use write() for EXIT: the CRT stdout FILE* may
		 * be corrupted by run_builtin's fflush/fstat on
		 * Windows pipes (S_ISFIFO not set, falls through
		 * to ENOSPC/ferror checks that damage CRT state).
		 * Raw write() to the dup2-restored fd is reliable.
		 */
		{
			char exit_buf[32];
			int len = snprintf(exit_buf, sizeof(exit_buf),
					   "EXIT %d\n", ret);
			if (write(STDOUT_FILENO, exit_buf, len) < 0)
				; /* best-effort */
		}
	}

	if (!single_shot)
		set_exit_intercept(NULL);
	strvec_clear(&args);
	strvec_clear(&env_vars);
	strvec_clear(&prev_env_keys);
	strbuf_release(&errpath);
	strbuf_release(&stdinpath);
	strbuf_release(&cwdpath);
	return 0;
}
