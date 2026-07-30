/*
 * Test process implementing the diff process protocol (diff.<driver>.process).
 *
 * Speaks the long-running process protocol over stdin/stdout and
 * answers command=hunks-by-oid requests from the blob object names
 * alone; no content is exchanged.  The --mode= switch selects the
 * response shape:
 *
 *   oid-fixed         packet: git< hunk 5 2 5 2
 *   oid-need-content  packet: git< status=need-content
 *
 * Success responses end with:
 *
 *   packet:          git< 0000
 *   packet:          git< status=success
 *   packet:          git< 0000
 *
 * Each request is logged to --log as:
 *
 *   command=<cmd> pathname=<path> old-oid=<hex> new-oid=<hex>
 */

#include "test-tool.h"
#include "pkt-line.h"
#include "parse-options.h"
#include "strbuf.h"

static FILE *logfile;

enum mode {
	MODE_OID_FIXED,
	MODE_OID_NEED_CONTENT,
};

static enum mode parse_mode(const char *s)
{
	if (!strcmp(s, "oid-fixed"))
		return MODE_OID_FIXED;
	if (!strcmp(s, "oid-need-content"))
		return MODE_OID_NEED_CONTENT;
	die("unknown --mode=%s", s);
}

/*
 * Read "key=value" packets up to a flush, capturing "command" and
 * "pathname".  Returns 1 if a request was read, 0 on EOF.
 *
 * The first packet uses the gentle variant so that a clean shutdown
 * by Git (EOF) does not produce a spurious "the remote end hung up
 * unexpectedly" on stderr.  Subsequent packets use the non-gentle
 * variant: once inside a request, truncation is a protocol violation
 * and dying loudly is the correct response.
 */
static int read_request_header(char **command, char **pathname,
			       char **old_oid, char **new_oid)
{
	int first = 1;
	char *line;

	*command = *pathname = *old_oid = *new_oid = NULL;
	for (;;) {
		const char *value;

		if (first) {
			if (packet_read_line_gently(0, NULL, &line) < 0)
				return 0;
			first = 0;
		} else {
			line = packet_read_line(0, NULL);
		}
		if (!line)
			break;
		if (skip_prefix(line, "command=", &value))
			*command = xstrdup(value);
		else if (skip_prefix(line, "pathname=", &value))
			*pathname = xstrdup(value);
		else if (skip_prefix(line, "old-oid=", &value))
			*old_oid = xstrdup(value);
		else if (skip_prefix(line, "new-oid=", &value))
			*new_oid = xstrdup(value);
	}
	return 1;
}

static void send_status(const char *status)
{
	packet_flush(1);
	packet_write_fmt(1, "%s\n", status);
	packet_flush(1);
}

static void command_loop(enum mode mode)
{
	for (;;) {
		char *command = NULL, *pathname = NULL;
		char *old_oid = NULL, *new_oid = NULL;

		if (!read_request_header(&command, &pathname,
					 &old_oid, &new_oid))
			break; /* EOF: Git closed its end */

		if (!command || strcmp(command, "hunks-by-oid"))
			die("unexpected command: '%s'",
			    command ? command : "(none)");

		if (logfile) {
			fprintf(logfile,
				"command=%s pathname=%s old-oid=%s new-oid=%s\n",
				command,
				pathname ? pathname : "(none)",
				old_oid ? old_oid : "(none)",
				new_oid ? new_oid : "(none)");
			fflush(logfile);
		}

		if (mode == MODE_OID_FIXED) {
			packet_write_fmt(1, "hunk 5 2 5 2\n");
			send_status("status=success");
		} else {
			send_status("status=need-content");
		}

		free(command);
		free(pathname);
		free(old_oid);
		free(new_oid);
	}
}

static void handshake(void)
{
	char *line;

	line = packet_read_line(0, NULL);
	if (!line || strcmp(line, "git-diff-client"))
		die("bad welcome: '%s'", line ? line : "(eof)");
	line = packet_read_line(0, NULL);
	if (!line || strcmp(line, "version=1"))
		die("bad version: '%s'", line ? line : "(eof)");
	if (packet_read_line(0, NULL))
		die("expected flush after version");

	packet_write_fmt(1, "git-diff-server\n");
	packet_write_fmt(1, "version=1\n");
	packet_flush(1);

	/* Drain capabilities advertised by Git */
	while ((line = packet_read_line(0, NULL)))
		; /* drain */

	packet_write_fmt(1, "capability=hunks-by-oid\n");
	packet_flush(1);
}

static const char *const usage_str[] = {
	"test-tool diff-process-backend --mode=<mode> [--log=<path>]",
	NULL
};

int cmd__diff_process_backend(int argc, const char **argv)
{
	const char *mode_str = NULL, *log_path = NULL;
	enum mode mode = MODE_OID_FIXED;
	struct option options[] = {
		OPT_STRING(0, "mode", &mode_str, "mode",
			   "response shape (default oid-fixed);"
			   " see the file header for the full list of modes"),
		OPT_STRING(0, "log", &log_path, "path",
			   "append per-request summary to this file"),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, options, usage_str, 0);
	if (argc)
		usage_with_options(usage_str, options);

	if (mode_str)
		mode = parse_mode(mode_str);

	if (log_path) {
		logfile = fopen(log_path, "a");
		if (!logfile)
			die_errno("failed to open log '%s'", log_path);
	}

	handshake();
	command_loop(mode);

	if (logfile && fclose(logfile))
		die_errno("error closing log");
	return 0;
}
