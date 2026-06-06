/*
 * test-batch-client.c: compiled replacement for the bash git()
 * wrapper in test-lib.sh's batch mode.
 *
 * Encodes the batch protocol (NUL-delimited, newline-terminated),
 * writes to GIT_BATCH_WR, reads response from GIT_BATCH_RD.
 * Falls back to direct git execution when batch is unavailable
 * or rejects the command.
 *
 * Environment variables (set by test-lib.sh):
 *   GIT_BATCH_RD      - fd number for reading daemon responses
 *   GIT_BATCH_WR      - fd number for writing commands to daemon
 *   GIT_BATCH_TMPDIR  - temp directory for stderr/stdin files
 *   GIT_BATCH_GIT     - path to real git binary (fallback)
 *   GIT_BATCH_CYGROOT - MSYS2 root path for path conversion (optional)
 */
#include "test-tool.h"
#include "strbuf.h"
#include "run-command.h"
#include "write-or-die.h"
#include "simple-ipc.h"
#include "pkt-line.h"
#include "copy.h"

extern char **environ;

static const char *whitelisted_cmds[] = {
	"add", "blame", "branch", "cat-file", "check-attr",
	"check-ignore", "checkout", "commit", "commit-tree",
	"config", "describe", "diff", "diff-files", "diff-index",
	"diff-tree", "for-each-ref", "grep", "hash-object",
	"log", "ls-files", "ls-tree", "merge-base", "mktag",
	"mktree", "name-rev", "notes", "read-tree", "reset",
	"rev-list", "rev-parse", "rm", "shortlog", "show",
	"show-ref", "status", "switch", "symbolic-ref", "tag",
	"update-index", "var", "verify-commit", "verify-tag",
	"version", "write-tree", NULL
};

static int is_whitelisted(const char *cmd)
{
	const char **p;
	for (p = whitelisted_cmds; *p; p++)
		if (!strcmp(*p, cmd))
			return 1;
	return 0;
}

static int should_forward_env(const char *name)
{
	return starts_with(name, "GIT_") ||
	       !strcmp(name, "HOME") ||
	       !strcmp(name, "EDITOR") ||
	       !strcmp(name, "VISUAL") ||
	       !strcmp(name, "FAKE_LINES") ||
	       !strcmp(name, "TERM");
}

static const char *cygroot;

static void convert_path(struct strbuf *out, const char *path)
{
	if (!cygroot) {
		strbuf_addstr(out, path);
		return;
	}
	if (path[0] == '/' && isalpha(path[1]) && path[2] == '/') {
		/* /c/foo -> C:/foo */
		strbuf_addch(out, toupper(path[1]));
		strbuf_addch(out, ':');
		strbuf_addstr(out, path + 2);
	} else if (path[0] == '/') {
		/* /tmp/foo -> C:/msys64/tmp/foo */
		strbuf_addstr(out, cygroot);
		strbuf_addstr(out, path);
	} else {
		strbuf_addstr(out, path);
	}
}

static int fallback_exec(const char *git, int argc, const char **argv,
			  const char *stdinfile)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	int i;

	strvec_push(&cp.args, git);
	for (i = 0; i < argc; i++)
		strvec_push(&cp.args, argv[i]);
	if (stdinfile) {
		int fd = open(stdinfile, O_RDONLY);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
	}
	return run_command(&cp);
}

static int encode_and_send(int wr_fd, int argc, const char **argv,
			    const char *errfile, const char *stdinfile)
{
	struct strbuf msg = STRBUF_INIT;
	struct strbuf pathbuf = STRBUF_INIT;
	struct strbuf cwd = STRBUF_INIT;
	ssize_t written;
	int i;

	/* Forward all relevant env vars */
	for (i = 0; environ[i]; i++) {
		const char *eq = strchr(environ[i], '=');
		if (!eq)
			continue;
		if (should_forward_env(environ[i])) {
			strbuf_addstr(&msg, "ENV=");
			strbuf_addstr(&msg, environ[i]);
			strbuf_addch(&msg, '\0');
		}
	}

	/* STDERR path */
	convert_path(&pathbuf, errfile);
	strbuf_addstr(&msg, "STDERR=");
	strbuf_addbuf(&msg, &pathbuf);
	strbuf_addch(&msg, '\0');

	/* STDIN path (if buffered) */
	if (stdinfile) {
		strbuf_reset(&pathbuf);
		convert_path(&pathbuf, stdinfile);
		strbuf_addstr(&msg, "STDIN=");
		strbuf_addbuf(&msg, &pathbuf);
		strbuf_addch(&msg, '\0');
	}

	/* CWD */
	strbuf_getcwd(&cwd);
	strbuf_reset(&pathbuf);
	convert_path(&pathbuf, cwd.buf);
	strbuf_addstr(&msg, "CWD=");
	strbuf_addbuf(&msg, &pathbuf);
	strbuf_addch(&msg, '\0');

	/* Command arguments */
	for (i = 0; i < argc; i++) {
		strbuf_addstr(&msg, argv[i]);
		strbuf_addch(&msg, '\0');
	}

	/* Terminator */
	strbuf_addch(&msg, '\n');

	written = write_in_full(wr_fd, msg.buf, msg.len);

	strbuf_release(&msg);
	strbuf_release(&pathbuf);
	strbuf_release(&cwd);

	return (written < 0) ? -1 : 0;
}

static int read_response(int rd_fd, const char *errfile)
{
	struct strbuf line = STRBUF_INIT;
	unsigned char ch;
	ssize_t r;

	for (;;) {
		strbuf_reset(&line);

		while ((r = read(rd_fd, &ch, 1)) == 1) {
			if (ch == '\n')
				break;
			strbuf_addch(&line, ch);
		}

		if (r <= 0 && !line.len) {
			strbuf_release(&line);
			return -2; /* EOF without EXIT */
		}

		if (starts_with(line.buf, "EXIT ")) {
			int code = atoi(line.buf + 5);
			strbuf_release(&line);

			/* Output buffered stderr */
			if (code >= 0) {
				struct stat st;
				if (!stat(errfile, &st) && st.st_size > 0) {
					int fd = open(errfile, O_RDONLY);
					if (fd >= 0) {
						copy_fd(fd, STDERR_FILENO);
						close(fd);
					}
				}
			}
			return code;
		}

		/* Regular output: pass through to stdout */
		write_in_full(STDOUT_FILENO, line.buf, line.len);
		write_in_full(STDOUT_FILENO, "\n", 1);
	}
}

/*
 * Parse the response from the IPC server.
 * Format: stdout output followed by "EXIT <code>\n".
 * Output lines go to our stdout, stderr from errfile.
 */
static int parse_ipc_response(const struct strbuf *answer,
			       const char *errfile)
{
	const char *p = answer->buf;
	const char *end = answer->buf + answer->len;
	const char *exit_line = NULL;
	int ret;

	/* Find the last "EXIT " line */
	{
		const char *search = answer->buf;
		while (search < end) {
			if (starts_with(search, "EXIT ")) {
				exit_line = search;
				break;
			}
			/* Skip to next line */
			search = memchr(search, '\n', end - search);
			if (!search)
				break;
			search++;
		}
	}

	if (!exit_line)
		return -2; /* No EXIT in response */

	/* Output everything before EXIT to stdout */
	if (exit_line > p)
		write_in_full(STDOUT_FILENO, p, exit_line - p);

	ret = atoi(exit_line + 5);

	/* Output buffered stderr */
	if (ret >= 0 && errfile) {
		struct stat st;
		if (!stat(errfile, &st) && st.st_size > 0) {
			int fd = open(errfile, O_RDONLY);
			if (fd >= 0) {
				copy_fd(fd, STDERR_FILENO);
				close(fd);
			}
		}
	}

	return ret;
}

int cmd__batch_client(int argc, const char **argv)
{
	const char *ipc_path, *rd_str, *wr_str, *tmpdir, *git;
	int ret;
	struct strbuf errfile = STRBUF_INIT;
	struct strbuf stdinfile = STRBUF_INIT;
	int has_stdin = 0;

	/* Skip "batch-client" argv[0] */
	argc--;
	argv++;

	if (!argc)
		die("batch-client: no command given");

	/* Read configuration from environment */
	ipc_path = getenv("GIT_BATCH_IPC");
	rd_str = getenv("GIT_BATCH_RD");
	wr_str = getenv("GIT_BATCH_WR");
	tmpdir = getenv("GIT_BATCH_TMPDIR");
	git = getenv("GIT_BATCH_GIT");
	cygroot = getenv("GIT_BATCH_CYGROOT");

	if (!git)
		git = "git";

#ifdef SUPPORTS_SIMPLE_IPC
	/*
	 * IPC mode: connect to daemon by pipe/socket name.
	 * Works on both Linux (Unix socket) and Windows (named pipe).
	 */
	if (ipc_path && tmpdir) {
		struct ipc_client_connect_options ipc_opts =
			IPC_CLIENT_CONNECT_OPTIONS_INIT;
		struct strbuf msg = STRBUF_INIT;
		struct strbuf answer = STRBUF_INIT;

		/* Handle control commands */
		if (!strcmp(argv[0], "--ping")) {
			strbuf_addstr(&msg, "ping\n");
			ret = ipc_client_send_command(ipc_path, &ipc_opts,
						      msg.buf, msg.len, &answer);
			strbuf_release(&msg);
			strbuf_release(&answer);
			return ret ? 1 : 0;
		}
		if (!strcmp(argv[0], "--quit")) {
			strbuf_addstr(&msg, "quit\n");
			ret = ipc_client_send_command(ipc_path, &ipc_opts,
						      msg.buf, msg.len, &answer);
			strbuf_release(&msg);
			strbuf_release(&answer);
			return ret ? 1 : 0;
		}

		/* Whitelist check */
		if (!is_whitelisted(argv[0])) {
			strbuf_release(&msg);
			strbuf_release(&answer);
			return fallback_exec(git, argc, argv, NULL);
		}

		/* GIT_REDIRECT bypass */
		if (getenv("GIT_REDIRECT_STDOUT") ||
		    getenv("GIT_REDIRECT_STDERR")) {
			strbuf_release(&msg);
			strbuf_release(&answer);
			return fallback_exec(git, argc, argv, NULL);
		}

		/* Create stderr temp file */
		strbuf_addf(&errfile, "%s/e.%d", tmpdir, (int)getpid());

		/* Buffer stdin if piped */
		if (!isatty(STDIN_FILENO)) {
			struct strbuf buf = STRBUF_INIT;
			strbuf_addf(&stdinfile, "%s/i.%d", tmpdir,
				    (int)getpid());
			if (strbuf_read(&buf, STDIN_FILENO, 0) > 0) {
				int fd = open(stdinfile.buf,
					      O_WRONLY | O_CREAT | O_TRUNC,
					      0644);
				if (fd >= 0) {
					write_in_full(fd, buf.buf, buf.len);
					close(fd);
					has_stdin = 1;
				}
			}
			strbuf_release(&buf);
		}

		/* Build protocol message */
		encode_and_send(-1, argc, argv, errfile.buf,
				has_stdin ? stdinfile.buf : NULL);
		/* encode_and_send wrote to a strbuf when wr_fd < 0 */
		/* Actually, we need to build msg here directly */
		strbuf_reset(&msg);
		{
			int i;
			struct strbuf pathbuf = STRBUF_INIT;
			struct strbuf cwd = STRBUF_INIT;

			/* Forward env vars */
			for (i = 0; environ[i]; i++) {
				if (should_forward_env(environ[i])) {
					strbuf_addstr(&msg, "ENV=");
					strbuf_addstr(&msg, environ[i]);
					strbuf_addch(&msg, '\0');
				}
			}
			/* STDERR */
			convert_path(&pathbuf, errfile.buf);
			strbuf_addstr(&msg, "STDERR=");
			strbuf_addbuf(&msg, &pathbuf);
			strbuf_addch(&msg, '\0');
			/* STDIN */
			if (has_stdin) {
				strbuf_reset(&pathbuf);
				convert_path(&pathbuf, stdinfile.buf);
				strbuf_addstr(&msg, "STDIN=");
				strbuf_addbuf(&msg, &pathbuf);
				strbuf_addch(&msg, '\0');
			}
			/* CWD */
			strbuf_getcwd(&cwd);
			strbuf_reset(&pathbuf);
			convert_path(&pathbuf, cwd.buf);
			strbuf_addstr(&msg, "CWD=");
			strbuf_addbuf(&msg, &pathbuf);
			strbuf_addch(&msg, '\0');
			/* Args */
			for (i = 0; i < argc; i++) {
				strbuf_addstr(&msg, argv[i]);
				strbuf_addch(&msg, '\0');
			}
			strbuf_addch(&msg, '\n');
			strbuf_release(&pathbuf);
			strbuf_release(&cwd);
		}

		/* Send via IPC and get response */
		ipc_opts.wait_if_busy = 1;
		ret = ipc_client_send_command(ipc_path, &ipc_opts,
					      msg.buf, msg.len, &answer);
		strbuf_release(&msg);

		if (ret) {
			/* IPC failed, fallback */
			ret = fallback_exec(git, argc, argv,
					    has_stdin ? stdinfile.buf : NULL);
		} else {
			ret = parse_ipc_response(&answer, errfile.buf);
			if (ret == -1 || ret == -2)
				ret = fallback_exec(git, argc, argv,
						    has_stdin ? stdinfile.buf
							     : NULL);
		}
		strbuf_release(&answer);
		unlink(errfile.buf);
		if (has_stdin)
			unlink(stdinfile.buf);
		strbuf_release(&errfile);
		strbuf_release(&stdinfile);
		return ret;
	}
#endif /* SUPPORTS_SIMPLE_IPC */

	/* Legacy fd-based mode (Linux coproc) */
	if (!rd_str || !wr_str || !tmpdir)
		return fallback_exec(git, argc, argv, NULL);

	{
		int rd_fd = atoi(rd_str);
		int wr_fd = atoi(wr_str);

		if (!is_whitelisted(argv[0]))
			return fallback_exec(git, argc, argv, NULL);
		if (getenv("GIT_REDIRECT_STDOUT") ||
		    getenv("GIT_REDIRECT_STDERR"))
			return fallback_exec(git, argc, argv, NULL);

		strbuf_addf(&errfile, "%s/e.%d", tmpdir, (int)getpid());

		if (!isatty(STDIN_FILENO)) {
			struct strbuf buf = STRBUF_INIT;
			strbuf_addf(&stdinfile, "%s/i.%d", tmpdir,
				    (int)getpid());
			if (strbuf_read(&buf, STDIN_FILENO, 0) > 0) {
				int fd = open(stdinfile.buf,
					      O_WRONLY | O_CREAT | O_TRUNC,
					      0644);
				if (fd >= 0) {
					write_in_full(fd, buf.buf, buf.len);
					close(fd);
					has_stdin = 1;
				}
			}
			strbuf_release(&buf);
		}

		ret = encode_and_send(wr_fd, argc, argv, errfile.buf,
				      has_stdin ? stdinfile.buf : NULL);
		if (ret < 0) {
			ret = fallback_exec(git, argc, argv,
					    has_stdin ? stdinfile.buf : NULL);
		} else {
			ret = read_response(rd_fd, errfile.buf);
			if (ret <= -1)
				ret = fallback_exec(git, argc, argv,
						    has_stdin ? stdinfile.buf
							     : NULL);
		}

		unlink(errfile.buf);
		if (has_stdin)
			unlink(stdinfile.buf);
		strbuf_release(&errfile);
		strbuf_release(&stdinfile);
		return ret;
	}
}
