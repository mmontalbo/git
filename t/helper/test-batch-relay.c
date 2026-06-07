/*
 * test-tool batch-relay: persistent bridge between bash and the
 * batch IPC daemon.
 *
 * Reads NUL-delimited, newline-terminated commands from stdin
 * (connected to bash via coproc), forwards each to the batch
 * daemon via simple-ipc named pipe, reads the response, and
 * writes it to stdout (back to bash).
 *
 * Eliminates per-command process spawn overhead: bash writes
 * directly to the relay's stdin (coproc fd, always inherited),
 * and the relay connects to the daemon by pipe name.
 *
 * Usage:
 *   test-tool batch-relay --ipc=<path>
 */
#include "test-tool.h"
#include "strbuf.h"
#include "simple-ipc.h"
#include "pkt-line.h"

int cmd__batch_relay(int argc, const char **argv)
{
#ifdef SUPPORTS_SIMPLE_IPC
	const char *ipc_path = NULL;
	struct strbuf request = STRBUF_INIT;
	struct strbuf answer = STRBUF_INIT;
	struct ipc_client_connect_options ipc_opts =
		IPC_CLIENT_CONNECT_OPTIONS_INIT;
	int i, c;

	for (i = 1; i < argc; i++) {
		if (skip_prefix(argv[i], "--ipc=", &ipc_path))
			continue;
	}
	if (!ipc_path)
		die("batch-relay: --ipc=<path> required");

	ipc_opts.wait_if_busy = 1;

	/*
	 * Read commands from stdin (bash coproc pipe).
	 * Each command is NUL-delimited, newline-terminated.
	 * Forward to IPC daemon, send response to stdout.
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

		/* Forward to daemon via IPC */
		strbuf_reset(&answer);
		if (ipc_client_send_command(ipc_path, &ipc_opts,
					    request.buf, request.len,
					    &answer)) {
			/* IPC failed - send EXIT -1 so bash falls back */
			fprintf(stdout, "EXIT -1\n");
			fflush(stdout);
			continue;
		}

		/* Write response to stdout (back to bash) */
		if (answer.len)
			fwrite(answer.buf, 1, answer.len, stdout);
		fflush(stdout);
	}

	strbuf_release(&request);
	strbuf_release(&answer);
	return 0;
#else
	die("batch-relay: simple-ipc not supported on this platform");
#endif
}
