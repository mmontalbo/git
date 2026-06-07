/*
 * test-tool batch-relay: persistent bridge between bash and the
 * batch daemon via a Unix socket persistent connection.
 *
 * Reads NUL-delimited, newline-terminated commands from stdin
 * (connected to bash via coproc), forwards each over the persistent
 * socket to the daemon, reads the response, writes to stdout.
 *
 * Unlike the per-command IPC approach, the socket connection is
 * opened ONCE at startup and kept alive (VS Code / LSP style).
 *
 * Usage:
 *   test-tool batch-relay --sock=<path>
 */
#include "test-tool.h"
#include "strbuf.h"
#include "unix-socket.h"

int cmd__batch_relay(int argc, const char **argv)
{
#ifndef NO_UNIX_SOCKETS
	const char *sock_path = NULL;
	int sock_fd, i, c;
	struct strbuf request = STRBUF_INIT;
	struct strbuf line = STRBUF_INIT;

	for (i = 1; i < argc; i++) {
		skip_prefix(argv[i], "--sock=", &sock_path) ||
		skip_prefix(argv[i], "--ipc=", &sock_path);
	}
	if (!sock_path)
		die("batch-relay: --sock=<path> required");

	/* Open log file for debugging */
	{
		const char *logdir = getenv("GIT_BATCH_TMPDIR");
		int logfd = -1;
		if (logdir) {
			struct strbuf lp = STRBUF_INIT;
			strbuf_addf(&lp, "%s/relay.log", logdir);
			logfd = open(lp.buf, O_WRONLY|O_CREAT|O_TRUNC, 0644);
			strbuf_release(&lp);
		}
#define RLOG(msg) do { if (logfd >= 0) write(logfd, msg, strlen(msg)); } while(0)
#define RLOGF(fmt, ...) do { if (logfd >= 0) { \
	char _b[256]; int _n = snprintf(_b, sizeof(_b), fmt, __VA_ARGS__); \
	if (_n > 0) write(logfd, _b, _n); } } while(0)

	RLOGF("relay: connecting to %s\n", sock_path);

	/* Connect with retries (daemon may not be listening yet) */
	for (i = 0; i < 50; i++) {
		sock_fd = unix_stream_connect(sock_path, 0);
		if (sock_fd >= 0)
			break;
		RLOGF("relay: connect attempt %d failed (errno=%d)\n", i, errno);
		usleep(100000); /* 100ms */
	}
	if (sock_fd < 0)
		die_errno("batch-relay: cannot connect to '%s' after retries", sock_path);

	RLOGF("relay: connected, sock_fd=%d\n", sock_fd);

	/*
	 * Relay loop: read command from stdin (bash), forward to
	 * daemon socket, read response, write to stdout (bash).
	 */
	while ((c = getchar()) != EOF) {
		strbuf_reset(&request);
		strbuf_addch(&request, c);

		/* Read until newline (end of command) */
		while ((c = getchar()) != EOF) {
			strbuf_addch(&request, c);
			if (c == '\n')
				break;
		}

		if (!request.len)
			break;

		RLOGF("relay: sending %d bytes\n", (int)request.len);

		/* Forward to daemon */
		if (write_in_full(sock_fd, request.buf, request.len) < 0) {
			RLOG("relay: write failed\n");
			fprintf(stdout, "EXIT -1\n");
			fflush(stdout);
			break;
		}

		RLOG("relay: reading response\n");

		/* Read response line by line until EXIT */
		strbuf_reset(&line);
		{
			char ch;
			ssize_t r;
			int saw_exit = 0;

			while ((r = read(sock_fd, &ch, 1)) == 1) {
				if (ch == '\n') {
					if (starts_with(line.buf, "EXIT ")) {
						RLOGF("relay: got %s\n", line.buf);
						fprintf(stdout, "%s\n", line.buf);
						fflush(stdout);
						saw_exit = 1;
						break;
					}
					fprintf(stdout, "%s\n", line.buf);
					strbuf_reset(&line);
				} else {
					strbuf_addch(&line, ch);
				}
			}
			if (!saw_exit) {
				RLOGF("relay: connection lost (r=%d)\n", (int)r);
				fprintf(stdout, "EXIT -1\n");
				fflush(stdout);
				break;
			}
		}
	}

	RLOG("relay: loop ended\n");
	if (logfd >= 0) close(logfd);
	}
#undef RLOG
#undef RLOGF
	close(sock_fd);
	strbuf_release(&request);
	strbuf_release(&line);
	return 0;
#else
	die("batch-relay: Unix sockets not supported on this platform");
#endif
}
