/*
 * Test backend for the long-running diff process protocol
 * (see diff-process.c and Documentation/gitattributes.adoc).
 *
 * Usage: test-tool diff-process-backend --mode=<mode> [--log=<path>]
 *
 * Implements the server side of the pkt-line handshake and a per-file
 * response loop.  The --mode= switch selects the response shape
 * (success, error, abort, crash, malformed hunks).
 *
 * Per-file request from Git:
 *
 *   packet:          git> command=hunks
 *   packet:          git> pathname=<path>
 *   packet:          git> [old-oid=<hex>]   (omitted for textconv/worktree)
 *   packet:          git> [new-oid=<hex>]
 *   packet:          git> 0000
 *   packet:          git> OLD_CONTENT
 *   packet:          git> 0000
 *   packet:          git> NEW_CONTENT
 *   packet:          git> 0000
 *
 * Response varies by --mode (default: whole-file):
 *
 *   whole-file   packet: git< hunk <1|0> <old_lines> <1|0> <new_lines>
 *                (start is 0 for an empty side, matching git diff)
 *   fixed-hunk   packet: git< hunk 5 2 5 2
 *   no-hunks     (no hunk packets)
 *   bad-hunk     packet: git< hunk 999 1 999 1
 *   bad-parse    packet: git< garbage not a hunk
 *   bad-sync     packet: git< hunk 1 2 1 1
 *   bad-gap      packet: git< hunk 1 1 3 1
 *   bad-start    packet: git< hunk 0 1 1 1
 *   noop-hunk    packet: git< hunk 1 0 1 0
 *   multi-hunk   packet: git< hunk 5 2 5 2
 *                packet: git< hunk 9 2 9 2
 *   insert       packet: git< hunk 3 0 3 2   (mid-file count-0 insertion)
 *   delete       packet: git< hunk 3 2 3 0   (mid-file count-0 deletion)
 *   flood        packet: git< hunk 1 1 1 1   (x100000)
 *   overlap      packet: git< hunk 1 5 1 5
 *                packet: git< hunk 3 2 3 2
 *   content-empty-packet packet: git< 0004 where a hunk line belongs
 *   content-empty-then-status packet: git< 0004, then status=success with no flush
 *   content-missing-status packet: git< 0000, then 0000 without status=
 *   no-cap       (omits capability=hunks during handshake)
 *   error        (status=error instead of status=success)
 *   abort        (status=abort instead of status=success)
 *   crash        exit(1) before sending any response
 *
 * The oid-* modes advertise hunks-by-oid and answer the no-content
 * object id request (command=hunks-by-oid, old-oid/new-oid, no content),
 * sending the two blob line counts first:
 *
 *   oid-fixed    packet: git< lines 10 10
 *                packet: git< hunk 5 2 5 2
 *   oid-no-hunks packet: git< lines 1 1          (blobs equivalent)
 *   oid-no-hunks-different-lines packet: git< lines 3 4
 *   oid-bad-hunk packet: git< lines 10 10
 *                packet: git< hunk 999 1 999 1   (past a 10-line blob)
 *   oid-huge-lines packet: git< lines 9999999999 1 (past MAX_XDIFF_SIZE)
 *   oid-bad-lines  packet: git< hunk 5 2 5 2       (missing lines header)
 *   oid-error    packet: git< lines 10 10, then status=error
 *   oid-abort    packet: git< lines 10 10, then status=abort
 *   oid-crash    packet: git< lines 10 10, then exit(1)
 *   oid-only     like oid-fixed, but advertises ONLY hunks-by-oid
 *
 * Well-formed success modes (not error/abort/crash/missing-status) end with:
 *
 *   packet:          git< 0000
 *   packet:          git< status=success
 *   packet:          git< 0000
 *
 * Each request is logged to --log as:
 *
 *   command=<cmd> pathname=<path> old-oid=<hex> new-oid=<hex> old=<first line> new=<first line>
 */

#include "test-tool.h"
#include "pkt-line.h"
#include "parse-options.h"
#include "strbuf.h"

static FILE *logfile;

enum mode {
	MODE_WHOLE_FILE,
	MODE_FIXED_HUNK,
	MODE_NO_HUNKS,
	MODE_BAD_HUNK,
	MODE_BAD_PARSE,
	MODE_BAD_SYNC,
	MODE_BAD_GAP,
	MODE_BAD_START,
	MODE_NOOP_HUNK,
	MODE_MULTI_HUNK,
	MODE_INSERT,
	MODE_DELETE,
	MODE_FLOOD,
	MODE_OVERLAP,
	MODE_CONTENT_EMPTY_PACKET,
	MODE_CONTENT_EMPTY_THEN_STATUS,
	MODE_CONTENT_MISSING_STATUS,
	MODE_NO_CAP,
	MODE_ERROR,
	MODE_ABORT,
	MODE_CRASH,
	MODE_OID_FIXED,
	MODE_OID_NO_HUNKS,
	MODE_OID_NO_HUNKS_DIFFERENT_LINES,
	MODE_OID_BAD_HUNK,
	MODE_OID_HUGE_LINES,
	MODE_OID_BAD_LINES,
	MODE_OID_ERROR,
	MODE_OID_ABORT,
	MODE_OID_CRASH,
	MODE_OID_ONLY,
};

static enum mode parse_mode(const char *s)
{
	if (!strcmp(s, "whole-file"))
		return MODE_WHOLE_FILE;
	if (!strcmp(s, "fixed-hunk"))
		return MODE_FIXED_HUNK;
	if (!strcmp(s, "no-hunks"))
		return MODE_NO_HUNKS;
	if (!strcmp(s, "bad-hunk"))
		return MODE_BAD_HUNK;
	if (!strcmp(s, "bad-parse"))
		return MODE_BAD_PARSE;
	if (!strcmp(s, "bad-sync"))
		return MODE_BAD_SYNC;
	if (!strcmp(s, "bad-gap"))
		return MODE_BAD_GAP;
	if (!strcmp(s, "bad-start"))
		return MODE_BAD_START;
	if (!strcmp(s, "noop-hunk"))
		return MODE_NOOP_HUNK;
	if (!strcmp(s, "multi-hunk"))
		return MODE_MULTI_HUNK;
	if (!strcmp(s, "insert"))
		return MODE_INSERT;
	if (!strcmp(s, "delete"))
		return MODE_DELETE;
	if (!strcmp(s, "flood"))
		return MODE_FLOOD;
	if (!strcmp(s, "overlap"))
		return MODE_OVERLAP;
	if (!strcmp(s, "content-empty-packet"))
		return MODE_CONTENT_EMPTY_PACKET;
	if (!strcmp(s, "content-empty-then-status"))
		return MODE_CONTENT_EMPTY_THEN_STATUS;
	if (!strcmp(s, "content-missing-status"))
		return MODE_CONTENT_MISSING_STATUS;
	if (!strcmp(s, "no-cap"))
		return MODE_NO_CAP;
	if (!strcmp(s, "error"))
		return MODE_ERROR;
	if (!strcmp(s, "oid-fixed"))
		return MODE_OID_FIXED;
	if (!strcmp(s, "oid-no-hunks"))
		return MODE_OID_NO_HUNKS;
	if (!strcmp(s, "oid-no-hunks-different-lines"))
		return MODE_OID_NO_HUNKS_DIFFERENT_LINES;
	if (!strcmp(s, "oid-bad-hunk"))
		return MODE_OID_BAD_HUNK;
	if (!strcmp(s, "oid-huge-lines"))
		return MODE_OID_HUGE_LINES;
	if (!strcmp(s, "oid-bad-lines"))
		return MODE_OID_BAD_LINES;
	if (!strcmp(s, "oid-error"))
		return MODE_OID_ERROR;
	if (!strcmp(s, "oid-abort"))
		return MODE_OID_ABORT;
	if (!strcmp(s, "oid-crash"))
		return MODE_OID_CRASH;
	if (!strcmp(s, "oid-only"))
		return MODE_OID_ONLY;
	if (!strcmp(s, "abort"))
		return MODE_ABORT;
	if (!strcmp(s, "crash"))
		return MODE_CRASH;
	die("unknown --mode=%s", s);
}

/*
 * Read "key=value" packets up to a flush, capturing "command" and
 * "pathname".  Returns 1 if a request was read, 0 on EOF.
 *
 * The first packet uses the gentle variant.  A clean shutdown by Git
 * (EOF) then produces no spurious "the remote end hung up unexpectedly"
 * on stderr.  Subsequent packets use the non-gentle
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

static size_t count_lines(const struct strbuf *buf)
{
	size_t lines = 0;

	for (size_t i = 0; i < buf->len; i++)
		if (buf->buf[i] == '\n')
			lines++;

	return lines + (buf->len > 0 && buf->buf[buf->len - 1] != '\n');
}

static void send_status(const char *status)
{
	packet_flush(1);
	packet_write_fmt(1, "%s\n", status);
	packet_flush(1);
}

/*
 * Answer a command=hunks-by-oid request.  The client sent no content.  This
 * function returns the "lines" total-count line first, which git needs to
 * validate the response, then a fixed answer.  oid-fixed reports lines 5-6
 * changed in a 10-line file; oid-no-hunks reports no change at all.
 */
static void respond_by_oid(enum mode mode)
{
	if (mode == MODE_OID_ERROR) {
		/* Well-framed reply that ends in a non-success status. */
		packet_write_fmt(1, "lines 10 10\n");
		send_status("status=error");
		return;
	}
	if (mode == MODE_OID_ABORT) {
		/* by-oid has no soft abort.  Git treats this as an error. */
		packet_write_fmt(1, "lines 10 10\n");
		send_status("status=abort");
		return;
	}
	if (mode == MODE_OID_CRASH) {
		/* Crash after the line-count header, before the hunk stream ends. */
		packet_write_fmt(1, "lines 10 10\n");
		exit(1);
	}
	if (mode == MODE_OID_HUGE_LINES) {
		/*
		 * A line count past MAX_XDIFF_SIZE.  Git must reject the header
		 * and fall back.  Frame an otherwise valid no-hunk reply, so
		 * dropping the bound would make blame wrongly report the pair
		 * equivalent (a visible mismatch) rather than block.
		 */
		packet_write_fmt(1, "lines 9999999999 1\n");
		send_status("status=success");
		return;
	}
	if (mode == MODE_OID_BAD_LINES) {
		/*
		 * A hunk line where the mandatory "lines <old> <new>" header
		 * belongs.  parse_lines_line() rejects it.  Frame the rest so a
		 * teardown races cleanly rather than blocking.
		 */
		packet_write_fmt(1, "hunk 5 2 5 2\n");
		send_status("status=success");
		return;
	}
	if (mode == MODE_OID_FIXED || mode == MODE_OID_ONLY) {
		packet_write_fmt(1, "lines 10 10\n");
		packet_write_fmt(1, "hunk 5 2 5 2\n");
	} else if (mode == MODE_OID_BAD_HUNK) {
		/* Well-formed, but the hunk lies past a 10-line blob. */
		packet_write_fmt(1, "lines 10 10\n");
		packet_write_fmt(1, "hunk 999 1 999 1\n");
	} else if (mode == MODE_OID_NO_HUNKS_DIFFERENT_LINES) {
		packet_write_fmt(1, "lines 3 4\n");
	} else {	/* MODE_OID_NO_HUNKS */
		packet_write_fmt(1, "lines 1 1\n");
	}
	send_status("status=success");
}

static void respond(enum mode mode,
		    const struct strbuf *old_buf,
		    const struct strbuf *new_buf)
{
	switch (mode) {
	case MODE_ERROR:
		send_status("status=error");
		return;
	case MODE_ABORT:
		send_status("status=abort");
		return;
	case MODE_CRASH:
		exit(1);
	case MODE_OID_FIXED:	/* content-mode fallback: same fixed hunk */
	case MODE_OID_BAD_HUNK:	/* likewise; only their by-oid answer is */
	case MODE_OID_HUGE_LINES: /* tested, so any well-formed reply serves */
	case MODE_OID_BAD_LINES: /* here (these by-oid modes never get a */
	case MODE_OID_ERROR:	/* content request) */
	case MODE_OID_ABORT:
	case MODE_OID_CRASH:
	case MODE_OID_ONLY:
	case MODE_FIXED_HUNK:
		packet_write_fmt(1, "hunk 5 2 5 2\n");
		break;
	case MODE_BAD_HUNK:
		packet_write_fmt(1, "hunk 999 1 999 1\n");
		break;
	case MODE_BAD_PARSE:
		packet_write_fmt(1, "garbage not a hunk\n");
		break;
	case MODE_BAD_SYNC:
		packet_write_fmt(1, "hunk 1 2 1 1\n");
		break;
	case MODE_BAD_GAP:
		/*
		 * Globally balanced: 1 changed line on each side, so the total
		 * unchanged counts match.  The gap before the change still
		 * differs between sides: old line 1 vs new line 3.  Exercises
		 * the per-gap lockstep-alignment check.
		 */
		packet_write_fmt(1, "hunk 1 1 3 1\n");
		break;
	case MODE_BAD_START:
		/*
		 * A start of 0 is valid only for an empty (count 0) range.
		 * Pairing it with a nonzero count names no line in either the
		 * protocol's or xdiff's coordinates.  The translation rejects
		 * it, and git falls back to the builtin diff.
		 */
		packet_write_fmt(1, "hunk 0 1 1 1\n");
		break;
	case MODE_NOOP_HUNK:
		packet_write_fmt(1, "hunk 1 0 1 0\n");
		break;
	case MODE_MULTI_HUNK:
		/*
		 * Two valid, non-overlapping, gap-aligned hunks.  Exercises
		 * the accepting branch of the per-gap lockstep check with a
		 * non-zero previous-hunk end (the realistic two-region case).
		 */
		packet_write_fmt(1, "hunk 5 2 5 2\n");
		packet_write_fmt(1, "hunk 9 2 9 2\n");
		break;
	case MODE_INSERT:
		/*
		 * A mid-file pure insertion (count 0 on the old side) in the
		 * protocol's 1-based-position form: 2 lines inserted before
		 * old line 3.  Exercises the count-0 path, which uses the
		 * unshifted position (not git diff's "-3,0" display start).
		 */
		packet_write_fmt(1, "hunk 3 0 3 2\n");
		break;
	case MODE_DELETE:
		/*
		 * A mid-file pure deletion (count 0 on the new side): old lines
		 * 3-4 removed, nothing added.  The lines after it are paired by
		 * position, so a divergent new side shows as context, not as a
		 * change.
		 */
		packet_write_fmt(1, "hunk 3 2 3 0\n");
		break;
	case MODE_FLOOD: {
		/*
		 * Emit far more hunks than any small file has lines, so Git
		 * trips its accumulation cap and falls back before reading
		 * them all.
		 */
		int i;
		for (i = 0; i < 100000; i++)
			packet_write_fmt(1, "hunk 1 1 1 1\n");
		break;
	}
	case MODE_OVERLAP:
		packet_write_fmt(1, "hunk 1 5 1 5\n");
		packet_write_fmt(1, "hunk 3 2 3 2\n");
		break;
	case MODE_CONTENT_EMPTY_PACKET:
		packet_write(1, "", 0);
		break;
	case MODE_CONTENT_EMPTY_THEN_STATUS:
		/*
		 * Send an empty packet where a hunk belongs, then the status
		 * with no flush between them.  A reader that mistakes the empty
		 * packet for a flush reads status=success and hides the change.
		 */
		packet_write(1, "", 0);
		packet_write_fmt(1, "status=success\n");
		packet_flush(1);
		return;
	case MODE_CONTENT_MISSING_STATUS:
		packet_flush(1);
		packet_flush(1);
		return;
	case MODE_OID_NO_HUNKS_DIFFERENT_LINES:
	case MODE_OID_NO_HUNKS:	/* content-mode fallback: no hunks */
	case MODE_NO_HUNKS:
		break;
	case MODE_NO_CAP:
	case MODE_WHOLE_FILE: {
		size_t old_lines = count_lines(old_buf);
		size_t new_lines = count_lines(new_buf);
		/*
		 * Match git diff output: start=0 when count=0
		 * (empty file side), 1 otherwise.
		 */
		packet_write_fmt(1, "hunk %"PRIuMAX" %"PRIuMAX
				 " %"PRIuMAX" %"PRIuMAX"\n",
				 (uintmax_t)(old_lines ? 1 : 0),
				 (uintmax_t)old_lines,
				 (uintmax_t)(new_lines ? 1 : 0),
				 (uintmax_t)new_lines);
		break;
	}
	}
	send_status("status=success");
}

static void command_loop(enum mode mode)
{
	for (;;) {
		char *command = NULL, *pathname = NULL;
		char *old_oid = NULL, *new_oid = NULL;
		struct strbuf obuf = STRBUF_INIT;
		struct strbuf nbuf = STRBUF_INIT;

		if (!read_request_header(&command, &pathname,
					 &old_oid, &new_oid))
			break; /* EOF: Git closed its end */

		if (command && !strcmp(command, "hunks-by-oid")) {
			/* An oid request carries no content. */
			if (logfile) {
				fprintf(logfile,
					"command=hunks-by-oid pathname=%s"
					" old-oid=%s new-oid=%s\n",
					pathname ? pathname : "(none)",
					old_oid ? old_oid : "(none)",
					new_oid ? new_oid : "(none)");
				fflush(logfile);
			}
			respond_by_oid(mode);
		} else {
			read_packetized_to_strbuf(0, &obuf, 0);
			read_packetized_to_strbuf(0, &nbuf, 0);

			if (logfile) {
				fprintf(logfile,
					"command=%s pathname=%s old-oid=%s new-oid=%s"
					" old=%.*s new=%.*s\n",
					command ? command : "(none)",
					pathname ? pathname : "(none)",
					old_oid ? old_oid : "(none)",
					new_oid ? new_oid : "(none)",
					(int)(strchrnul(obuf.buf, '\n') - obuf.buf),
					obuf.buf,
					(int)(strchrnul(nbuf.buf, '\n') - nbuf.buf),
					nbuf.buf);
				fflush(logfile);
			}

			respond(mode, &obuf, &nbuf);
		}

		free(command);
		free(pathname);
		free(old_oid);
		free(new_oid);
		strbuf_release(&obuf);
		strbuf_release(&nbuf);
	}
}

static void handshake(enum mode mode)
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

	/*
	 * Respond with our capabilities.  no-cap advertises none; oid-only
	 * advertises hunks-by-oid but not hunks (a diff process that answers only by
	 * object id).
	 */
	if (mode != MODE_NO_CAP && mode != MODE_OID_ONLY)
		packet_write_fmt(1, "capability=hunks\n");
	if (mode == MODE_OID_FIXED || mode == MODE_OID_NO_HUNKS ||
	    mode == MODE_OID_NO_HUNKS_DIFFERENT_LINES ||
	    mode == MODE_OID_BAD_HUNK || mode == MODE_OID_HUGE_LINES ||
	    mode == MODE_OID_BAD_LINES || mode == MODE_OID_ERROR ||
	    mode == MODE_OID_ABORT || mode == MODE_OID_CRASH ||
	    mode == MODE_OID_ONLY)
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
	enum mode mode = MODE_WHOLE_FILE;
	struct option options[] = {
		OPT_STRING(0, "mode", &mode_str, "mode",
			   "response shape (default whole-file);"
			   " see the file header for the full list of modes"),
		OPT_STRING(0, "log", &log_path, "path",
			   "append per-request summary to this file"),
		OPT_END()
	};

	/*
	 * Git may tear the connection down mid-reply (for example on a
	 * rejected by-oid response); ignore SIGPIPE so the backend exits
	 * cleanly instead of dying by signal.
	 */
	signal(SIGPIPE, SIG_IGN);

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

	handshake(mode);
	command_loop(mode);

	if (logfile && fclose(logfile))
		die_errno("error closing log");
	return 0;
}
