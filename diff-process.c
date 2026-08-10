/*
 * Diff process backend: communicates with a long-running external
 * diff process via the pkt-line protocol to obtain custom line-matching
 * results.  The process controls which lines are marked as changed
 * while the display shows the file content (after any textconv
 * transformation, if configured).
 *
 * Protocol: pkt-line over stdin/stdout, following the pattern of
 * the long-running filter process protocol (see convert.c).
 *
 * Handshake:
 *   git> git-diff-client / version=1 / flush
 *   process< git-diff-server / version=1 / flush
 *   git> capability=hunks / [capability=hunks-by-oid] / flush
 *   process< <supported capabilities> / flush
 *
 * Per-file (command=hunks), sending both blobs' content:
 *   git> command=hunks / pathname=<path> / [old-oid=<hex>] / [new-oid=<hex>] / flush
 *   git> <old content packetized> / flush
 *   git> <new content packetized> / flush
 *   process< hunk <old_start> <old_count> <new_start> <new_count>
 *   process< ... / flush
 *   process< status=success / flush
 *
 * Per-file (command=hunks-by-oid), sending no content: the process answers
 * about the blob pair by object id alone.  It replies with the two blobs'
 * line counts first, so git can validate the hunks without loading a blob:
 *   git> command=hunks-by-oid / pathname=<path> / old-oid=<hex> / new-oid=<hex> / flush
 *   process< lines <old_count> <new_count>
 *   process< hunk <old_start> <old_count> <new_start> <new_count>
 *   process< ... / flush
 *   process< status=success / flush
 *
 * When the process returns no hunks with status=success, it reports
 * the files equivalent.  Git will skip the diff for that file.
 */

#include "git-compat-util.h"
#include "diff-process.h"
#include "diff.h"
#include "gettext.h"
#include "hex.h"
#include "repository.h"
#include "sigchain.h"
#include "userdiff.h"
#include "sub-process.h"
#include "pkt-line.h"
#include "strbuf.h"
#include "xdiff-interface.h"
#include "xdiff/xdiff.h"

#define CAP_HUNKS (1u << 0)
#define CAP_HUNKS_BY_OID (1u << 1)

struct diff_subprocess {
	struct subprocess_entry subprocess;
	unsigned int supported_capabilities;
};

static int start_diff_process_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = { 1, 0 };
	static struct subprocess_capability capabilities[] = {
		{ "hunks", CAP_HUNKS },
		{ "hunks-by-oid", CAP_HUNKS_BY_OID },
		{ NULL, 0 }
	};
	struct diff_subprocess *entry =
		container_of(subprocess, struct diff_subprocess, subprocess);

	return subprocess_handshake(subprocess, "git-diff",
				    versions, NULL,
				    capabilities,
				    &entry->supported_capabilities);
}

static struct diff_subprocess *get_or_launch_process(
		struct userdiff_driver *drv)
{
	struct diff_subprocess *entry;

	/*
	 * A prior failure drops the process for the session.  Callers check
	 * this too; keep the guard here so no path relaunches a failed driver.
	 */
	if (drv->diff_process_failed)
		return NULL;
	if (drv->diff_subprocess)
		return drv->diff_subprocess;

	entry = xcalloc(1, sizeof(*entry));
	if (subprocess_start_command(&entry->subprocess, drv->process,
				     start_diff_process_fn)) {
		free(entry);
		drv->diff_process_failed = 1;
		return NULL;
	}

	drv->diff_subprocess = entry;
	return entry;
}

static void diff_process_mark_failed(struct userdiff_driver *drv)
{
	struct diff_subprocess *backend;

	drv->diff_process_failed = 1;
	backend = drv->diff_subprocess;
	if (!backend)
		return;

	drv->diff_subprocess = NULL;
	subprocess_stop_command(&backend->subprocess);
	free(backend);
}

static int send_file_content(int fd, const char *buf, long size)
{
	int ret = 0;

	if (size < 0)
		return -1;
	if (size > 0)
		ret = write_packetized_from_buf_no_flush(buf, size, fd);
	if (ret)
		return ret;
	return packet_flush_gently(fd);
}

/*
 * A hunk in the diff process's presentation coordinates: the line
 * numbering it reports over the protocol.  Kept distinct from
 * xdl_hunk_t (xdiff's coordinates) so that only translated hunks ever
 * reach the diff algorithm; diff_process_hunk_to_xdl() is the single
 * crossing point.
 */
struct diff_process_hunk {
	long old_start, old_count;
	long new_start, new_count;
};

/*
 * Parse one non-negative decimal field of a hunk line into *out and
 * advance *line past it.  Fields must be plain decimal with no leading
 * whitespace or sign (isdigit() takes an unsigned char to stay defined
 * for high-bit bytes).  The first three fields are followed by a single
 * space; the last (is_last) is followed by end-of-string or a space.
 * parse_hunk_field() allows and ignores trailing space-separated tokens
 * after the last field.  A future protocol version can then append fields
 * (e.g. a "moved" marker) without an older diff process rejecting the line.
 * This mirrors the request-side rule that a diff process ignores unknown keys.
 */
static int parse_hunk_field(const char **line, long *out, int is_last)
{
	const char *p = *line;
	char *end;

	if (!isdigit((unsigned char)*p))
		return -1;
	errno = 0;
	*out = strtol(p, &end, 10);
	if (errno || end == p)
		return -1;
	if (is_last) {
		if (*end != '\0' && *end != ' ')
			return -1;
	} else {
		if (*end != ' ')
			return -1;
		end++;
	}
	*line = end;
	return 0;
}

static int parse_hunk_line(const char *line,
			   struct diff_process_hunk *presented)
{
	/* Format: "hunk <old_start> <old_count> <new_start> <new_count>" */
	if (!skip_prefix(line, "hunk ", &line))
		return -1;
	if (parse_hunk_field(&line, &presented->old_start, 0) ||
	    parse_hunk_field(&line, &presented->old_count, 0) ||
	    parse_hunk_field(&line, &presented->new_start, 0) ||
	    parse_hunk_field(&line, &presented->new_count, 1))
		return -1;
	return 0;
}

/*
 * Parse the "lines <old> <new>" header a hunks-by-oid response sends before
 * its hunks: two non-negative decimal counts.  Returns 0 with *old_lines and
 * *new_lines set, -1 otherwise.
 */
static int parse_lines_line(const char *line, long *old_lines, long *new_lines)
{
	if (!skip_prefix(line, "lines ", &line))
		return -1;
	if (parse_hunk_field(&line, old_lines, 0) ||
	    parse_hunk_field(&line, new_lines, 1))
		return -1;
	return 0;
}

/*
 * Translate a hunk from the diff process's presentation coordinates
 * into xdiff's.
 *
 * Protocol starts are already 1-based positions (the line a change sits
 * before), the same numbering xdiff uses.  The only adjustment is for an
 * empty file side.  "git diff" addresses an empty side with a start of 0
 * and a count of 0 (e.g. "0 0 1 5" adds five lines to an empty old side).
 * xdiff uses start-1 as an array index, so that 0 becomes 1 here.
 *
 * This is NOT the full inverse of xdl_emit_hunk_hdr() (xdiff/xutils.c).
 * That emitter shifts a count-0 range to start-1 for the displayed "@@"
 * header.  The protocol instead keeps the unshifted 1-based position for a
 * mid-file insert or delete.  This is the single point where presentation
 * coordinates become xdiff coordinates, so xdl_populate_hunks_from_external()
 * may assume 1-based starts.
 *
 * Returns -1 for a start of 0 paired with a nonzero count, which names
 * no line in either coordinate system.  (parse_hunk_line() already
 * guarantees non-negative starts and counts.)
 */
static int diff_process_hunk_to_xdl(const struct diff_process_hunk *presented,
				    xdl_hunk_t *xdl)
{
	long old_start = presented->old_start;
	long new_start = presented->new_start;

	if ((!old_start && presented->old_count) ||
	    (!new_start && presented->new_count))
		return -1;
	if (!old_start)
		old_start = 1;
	if (!new_start)
		new_start = 1;

	xdl->old_start = old_start;
	xdl->old_count = presented->old_count;
	xdl->new_start = new_start;
	xdl->new_count = presented->new_count;
	return 0;
}

/*
 * Outcome of one exchange with the diff process, internal to this
 * file.  The public interface collapses HUNKS and SKIP into "proceed"
 * (see enum diff_process_result); only fill_hunks() needs to tell a
 * success (which may carry hunks) apart from a skip.
 */
enum diff_process_reply {
	DIFF_PROCESS_REPLY_HUNKS,	/* success; *hunks_out set, count may be 0 */
	DIFF_PROCESS_REPLY_SKIP,	/* local skip, no capability, or withdrawal */
	DIFF_PROCESS_REPLY_ERROR,	/* process or communication failure */
};

/*
 * Read hunk lines until the flush packet, translating each into xdiff
 * coordinates and appending it to *hunks_out.  Cap the count at max_hunks so
 * a process that floods hunk lines cannot grow memory without bound.  Returns 0
 * at the flush, -1 on a framing, parse, or overflow error; *hunks_out is set
 * on every path so the caller frees it.
 */
static int read_hunks(int fd_out, size_t max_hunks, const char *process,
		      const char *path, xdl_hunk_t **hunks_out, size_t *nr_out)
{
	xdl_hunk_t *hunks = NULL, hunk;
	struct diff_process_hunk presented;
	size_t nr = 0, alloc = 0;
	char *line = packet_buffer;
	int len, ret = 0;

	for (;;) {
		enum packet_read_status status;

		status = packet_read_with_status(fd_out, NULL, NULL,
						 line, LARGE_PACKET_MAX, &len,
						 PACKET_READ_CHOMP_NEWLINE |
						 PACKET_READ_GENTLE_ON_EOF);
		if (status == PACKET_READ_FLUSH)
			break;
		if (status != PACKET_READ_NORMAL) {
			ret = -1;
			break;
		}
		if (parse_hunk_line(line, &presented) < 0 ||
		    diff_process_hunk_to_xdl(&presented, &hunk) < 0) {
			ret = -1;
			break;
		}
		if (nr >= max_hunks) {
			warning(_("diff process '%s' sent too many hunks for"
				  " '%s'"), process, path);
			ret = -1;
			break;
		}
		ALLOC_GROW(hunks, nr + 1, alloc);
		hunks[nr++] = hunk;
	}
	*hunks_out = hunks;
	*nr_out = nr;
	return ret;
}

static enum diff_process_reply get_hunks(
		struct userdiff_driver *drv,
		const char *path,
		const char *old_buf, long old_size,
		const char *new_buf, long new_size,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		xdl_hunk_t **hunks_out,
		size_t *nr_hunks_out)
{
	struct diff_subprocess *backend;
	struct child_process *process;
	int fd_in, fd_out;
	struct strbuf status = STRBUF_INIT;
	xdl_hunk_t *hunks = NULL;
	size_t nr_hunks = 0;
	size_t max_hunks;

	backend = get_or_launch_process(drv);
	if (!backend)
		return DIFF_PROCESS_REPLY_ERROR;

	if (!(backend->supported_capabilities & CAP_HUNKS))
		return DIFF_PROCESS_REPLY_SKIP;

	process = subprocess_get_child_process(&backend->subprocess);
	fd_in = process->in;
	fd_out = process->out;

	sigchain_push(SIGPIPE, SIG_IGN);

	/*
	 * The path does not fit one pkt-line payload, so Git skips this file.
	 * The process stays synchronized because Git sends no request.
	 */
	if (strlen(path) > LARGE_PACKET_DATA_MAX - strlen("pathname=\n")) {
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_REPLY_SKIP;
	}

	/* Send request */
	if (packet_write_fmt_gently(fd_in, "command=hunks\n") ||
	    packet_write_fmt_gently(fd_in, "pathname=%s\n", path))
		goto comm_error;
	/*
	 * old-oid/new-oid let the process key a cache on the blob pair.  The
	 * caller sends a side only when its content is the raw blob, and passes
	 * NULL otherwise (e.g. for textconv'd content).  A present oid then
	 * names the exact bytes the process receives.
	 */
	if (oid_a &&
	    packet_write_fmt_gently(fd_in, "old-oid=%s\n", oid_to_hex(oid_a)))
		goto comm_error;
	if (oid_b &&
	    packet_write_fmt_gently(fd_in, "new-oid=%s\n", oid_to_hex(oid_b)))
		goto comm_error;
	if (packet_flush_gently(fd_in))
		goto comm_error;

	/* Send old file content */
	if (send_file_content(fd_in, old_buf, old_size))
		goto comm_error;

	/* Send new file content */
	if (send_file_content(fd_in, new_buf, new_size))
		goto comm_error;

	/*
	 * A misbehaving process can flood hunk lines, so cap the accumulation
	 * before validation to bound memory.  The two files' combined byte size
	 * is a safe cap: it exceeds any real hunk count.  A useful hunk covers at
	 * least one line, but count-0 no-op hunks cover none, so the count is not
	 * strictly bounded by distinct lines; the byte-size cap bounds it anyway.
	 */
	max_hunks = (size_t)old_size + (size_t)new_size + 1;

	if (read_hunks(fd_out, max_hunks, drv->process, path, &hunks, &nr_hunks))
		goto comm_error;

	/* Read status */
	if (subprocess_read_status(fd_out, &status))
		goto comm_error;
	/*
	 * The status block must include a status= packet.  An empty block is
	 * missing status framing, so Git drops the process for the session.
	 */
	if (!status.len)
		goto comm_error;

	if (!strcmp(status.buf, "success")) {
		*hunks_out = hunks;
		*nr_hunks_out = nr_hunks;
		strbuf_release(&status);
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_REPLY_HUNKS;
	}

	if (!strcmp(status.buf, "abort")) {
		/*
		 * The process voluntarily withdrew: stop sending requests
		 * but do not warn (this is not a failure).
		 */
		backend->supported_capabilities &= ~CAP_HUNKS;
		free(hunks);
		strbuf_release(&status);
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_REPLY_SKIP;
	}

	/* status=error or unknown status */
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_REPLY_ERROR;

comm_error:
	/*
	 * Communication failure (broken pipe, malformed response).
	 * Tear down the process and mark as failed so we do not
	 * retry on every subsequent file.
	 */
	diff_process_mark_failed(drv);
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_REPLY_ERROR;
}

/*
 * Whether exactly one of the two blobs ends in a newline.  A change
 * that only adds or removes the trailing newline is not expressible as
 * line hunks, so a process comparing lines reports the files equivalent.
 */
static int eof_newline_differs(const mmfile_t *a, const mmfile_t *b)
{
	int a_nl = a->size > 0 && a->ptr[a->size - 1] == '\n';
	int b_nl = b->size > 0 && b->ptr[b->size - 1] == '\n';
	return a_nl != b_nl;
}

/*
 * Number of lines in a blob, matching xdiff's record count: one per
 * newline, plus one more if the last line has no trailing newline.
 */
static long count_lines(const char *buf, long size)
{
	long lines = 0, i;

	for (i = 0; i < size; i++)
		if (buf[i] == '\n')
			lines++;
	if (size > 0 && buf[size - 1] != '\n')
		lines++;
	return lines;
}

/*
 * Validate the process's hunks (already in xdiff coordinates) against the
 * two blobs before they bypass the diff algorithm.  Each hunk must fit
 * within its file, the hunks must be ordered and non-overlapping, and
 * the unchanged run before each hunk (and after the last) must be the
 * same length on both sides.  xdl_build_script() walks the two files
 * in lockstep over unchanged lines, so a mismatched gap desynchronizes
 * it and yields a corrupt diff even when the totals balance.  This is
 * the git layer's job so xdiff stays diagnostic-free; on a bad response
 * we warn and the caller falls back to the builtin diff.  Returns 0 if
 * valid, -1 (after warning) otherwise.
 */
static int validate_external_hunks(const xdl_hunk_t *hunks, size_t nr,
				   long old_lines, long new_lines,
				   const char *process, const char *path)
{
	size_t i;
	long prev_old_end = 0, prev_new_end = 0;

	for (i = 0; i < nr; i++) {
		const xdl_hunk_t *h = &hunks[i];

		/* A hunk must describe a change; reject one that names none. */
		if (!h->old_count && !h->new_count) {
			warning(_("diff process '%s' returned a hunk with no "
				  "change for '%s'; using the builtin diff"),
				process, path);
			return -1;
		}
		if (h->old_count > old_lines - h->old_start + 1 ||
		    h->new_count > new_lines - h->new_start + 1) {
			warning(_("diff process '%s' returned a hunk past the "
				  "end of '%s'; using the builtin diff"),
				process, path);
			return -1;
		}
		if (h->old_start < prev_old_end || h->new_start < prev_new_end) {
			warning(_("diff process '%s' returned overlapping hunks "
				  "for '%s'; using the builtin diff"),
				process, path);
			return -1;
		}
		if (h->old_start - prev_old_end != h->new_start - prev_new_end)
			goto misaligned;
		prev_old_end = h->old_start + h->old_count;
		prev_new_end = h->new_start + h->new_count;
	}
	if (old_lines - prev_old_end != new_lines - prev_new_end)
		goto misaligned;
	return 0;

misaligned:
	warning(_("diff process '%s' returned hunks that leave '%s' "
		  "misaligned; using the builtin diff"),
		process, path);
	return -1;
}

enum diff_process_result diff_process_fill_hunks(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		const mmfile_t *file_a,
		const mmfile_t *file_b,
		xpparam_t *xpp)
{
	struct userdiff_driver *drv;
	xdl_hunk_t *ext_hunks = NULL;
	size_t nr = 0;
	enum diff_process_reply reply;

	if (!diffopt || !path)
		return DIFF_PROCESS_DIFFED;
	if (diffopt->flags.no_diff_process || diffopt->ignore_driver_algorithm)
		return DIFF_PROCESS_DIFFED;
	/*
	 * Whitespace-ignoring, regex-ignore (-I) and anchored options
	 * change which lines count as different, but the process is never
	 * told about them, so its hunks could not honor them.  Rather
	 * than silently override the user's request, fall back to the
	 * builtin diff, which does honor these flags.  Key this off xpp
	 * (the parameters this diff actually runs with) rather than diffopt.
	 * A caller like blame keeps its flags outside diffopt, so xpp covers
	 * it without a separate guard.
	 */
	if ((xpp->flags & (XDF_WHITESPACE_FLAGS | XDF_IGNORE_BLANK_LINES)) ||
	    xpp->ignore_regex_nr || xpp->anchors_nr)
		return DIFF_PROCESS_DIFFED;

	drv = userdiff_find_by_path(diffopt->repo->index, path);
	if (!drv || !drv->process)
		return DIFF_PROCESS_DIFFED;
	if (drv->diff_process_failed)
		return DIFF_PROCESS_DIFFED;

	reply = get_hunks(drv, path,
			  file_a->ptr, file_a->size,
			  file_b->ptr, file_b->size,
			  oid_a, oid_b,
			  &ext_hunks, &nr);
	if (reply == DIFF_PROCESS_REPLY_ERROR) {
		warning(_("diff process '%s' failed for '%s',"
			  " falling back to builtin diff"),
			drv->process, path);
		return DIFF_PROCESS_ERROR;
	}
	if (reply == DIFF_PROCESS_REPLY_SKIP)
		return DIFF_PROCESS_DIFFED;

	/* DIFF_PROCESS_REPLY_HUNKS: the process responded with success. */
	if (!nr) {
		free(ext_hunks);
		/*
		 * No hunks means the process reports the line content
		 * identical, but it cannot express a trailing-newline-only
		 * change.  When that is the actual difference, fall back to the
		 * builtin diff.  The builtin diff keeps the "\ No newline at
		 * end of file" marker instead of reporting the files equivalent.
		 */
		if (eof_newline_differs(file_a, file_b))
			return DIFF_PROCESS_DIFFED;
		return DIFF_PROCESS_EQUIVALENT;
	}
	if (validate_external_hunks(ext_hunks, nr,
				    count_lines(file_a->ptr, file_a->size),
				    count_lines(file_b->ptr, file_b->size),
				    drv->process, path) < 0) {
		free(ext_hunks);
		return DIFF_PROCESS_DIFFED;
	}
	xpp->external_hunks = ext_hunks;
	xpp->external_hunks_nr = nr;
	return DIFF_PROCESS_DIFFED;
}

void diff_process_clear_hunks(xpparam_t *xpp)
{
	FREE_AND_NULL(xpp->external_hunks);
	xpp->external_hunks_nr = 0;
}

static int path_uses_textconv(struct diff_options *diffopt, const char *path)
{
	struct userdiff_driver *drv;

	if (!path)
		return 0;
	drv = userdiff_find_by_path(diffopt->repo->index, path);
	return drv && drv->textconv;
}

int diff_process_by_oid_eligible(struct diff_options *diffopt,
				 const char *old_path,
				 const char *new_path,
				 int xdl_opts)
{
	if (!diffopt || !diffopt->repo)
		return 0;
	if (xdl_opts & (XDF_WHITESPACE_FLAGS | XDF_IGNORE_BLANK_LINES))
		return 0;
	if (diffopt->ignore_regex_nr || diffopt->anchors_nr)
		return 0;
	if (diffopt->flags.allow_textconv &&
	    (path_uses_textconv(diffopt, old_path) ||
	     path_uses_textconv(diffopt, new_path)))
		return 0;
	return 1;
}

/*
 * Request hunks by object id, sending no content.  The process sends its two
 * blob line counts first, as one "lines <old> <new>" line, then the hunk
 * lines.  The up-front counts let git validate the hunks without loading a
 * blob.  They also cap the hunk count, since a valid response has no more
 * hunks than the files have lines.  Returns HUNKS on success, SKIP when the
 * process does not advertise the capability, or ERROR on a protocol failure.
 */
static enum diff_process_reply get_hunks_by_oid(
		struct userdiff_driver *drv, const char *path,
		const struct object_id *oid_a, const struct object_id *oid_b,
		xdl_hunk_t **hunks_out, size_t *nr_hunks_out,
		long *old_lines, long *new_lines)
{
	struct diff_subprocess *backend;
	struct child_process *process;
	int fd_in, fd_out, len;
	struct strbuf status = STRBUF_INIT;
	xdl_hunk_t *hunks = NULL;
	size_t nr_hunks = 0;
	char *line;

	backend = get_or_launch_process(drv);
	if (!backend)
		return DIFF_PROCESS_REPLY_ERROR;
	if (!(backend->supported_capabilities & CAP_HUNKS_BY_OID))
		return DIFF_PROCESS_REPLY_SKIP;

	process = subprocess_get_child_process(&backend->subprocess);
	fd_in = process->in;
	fd_out = process->out;
	sigchain_push(SIGPIPE, SIG_IGN);

	/*
	 * The path does not fit one pkt-line payload, so Git skips this file.
	 * The process stays synchronized because Git sends no request.
	 */
	if (strlen(path) > LARGE_PACKET_DATA_MAX - strlen("pathname=\n")) {
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_REPLY_SKIP;
	}

	if (packet_write_fmt_gently(fd_in, "command=hunks-by-oid\n") ||
	    packet_write_fmt_gently(fd_in, "pathname=%s\n", path) ||
	    packet_write_fmt_gently(fd_in, "old-oid=%s\n", oid_to_hex(oid_a)) ||
	    packet_write_fmt_gently(fd_in, "new-oid=%s\n", oid_to_hex(oid_b)) ||
	    packet_flush_gently(fd_in))
		goto comm_error;
	/* Deliberately no content follows. */

	/*
	 * The two blob line counts come first, before any hunk.  Reject counts
	 * larger than any blob xdiff would accept: no real blob has that many
	 * lines.  The bound also keeps the hunk cap and the validation arithmetic
	 * (which trust these counts) free of overflow.
	 */
	len = packet_read_line_gently(fd_out, NULL, &line);
	if (len < 0 || !line || parse_lines_line(line, old_lines, new_lines) < 0)
		goto comm_error;
	/* Non-negative per parse_hunk_field(); cast for the unsigned bound. */
	if ((unsigned long)*old_lines > MAX_XDIFF_SIZE ||
	    (unsigned long)*new_lines > MAX_XDIFF_SIZE)
		goto comm_error;

	if (read_hunks(fd_out, (size_t)*old_lines + (size_t)*new_lines + 1,
		       drv->process, path, &hunks, &nr_hunks))
		goto comm_error;

	if (subprocess_read_status(fd_out, &status))
		goto comm_error;

	if (!strcmp(status.buf, "success")) {
		*hunks_out = hunks;
		*nr_hunks_out = nr_hunks;
		strbuf_release(&status);
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_REPLY_HUNKS;
	}

	/*
	 * Any non-success status (including "abort") is an error here; the
	 * by-oid path has no per-request soft-decline like get_hunks().  The
	 * caller warns once.  Drop the process for the session, since blame
	 * and "git log -L" consult once per commit (see gitattributes.adoc).
	 */
	free(hunks);
	strbuf_release(&status);
	diff_process_mark_failed(drv);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_REPLY_ERROR;

comm_error:
	diff_process_mark_failed(drv);
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_REPLY_ERROR;
}

int diff_process_hunks_by_oid(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		xdl_emit_hunk_consume_func_t hunk_func,
		void *cb_data)
{
	struct userdiff_driver *drv;
	xdl_hunk_t *hunks = NULL;
	size_t nr = 0, i;
	long old_lines = 0, new_lines = 0;
	enum diff_process_reply reply;

	if (!diffopt || !path || !oid_a || !oid_b)
		return 0;
	if (diffopt->flags.no_diff_process || diffopt->ignore_driver_algorithm)
		return 0;
	drv = userdiff_find_by_path(diffopt->repo->index, path);
	if (!drv || !drv->process || drv->diff_process_failed)
		return 0;

	reply = get_hunks_by_oid(drv, path, oid_a, oid_b,
				 &hunks, &nr, &old_lines, &new_lines);
	if (reply == DIFF_PROCESS_REPLY_SKIP)
		return 0;	/* Git skips the process for this file. */
	if (reply == DIFF_PROCESS_REPLY_ERROR) {
		/*
		 * blame and "git log -L" consult by object id once per commit
		 * over history.  Warn once, then drop the process for the
		 * session so a persistent error does not warn on every commit.
		 * Marking the process here is idempotent and covers startup
		 * failure.
		 */
		warning(_("diff process '%s' failed answering by object id"
			  " for '%s'"), drv->process, path);
		diff_process_mark_failed(drv);
		return 0;	/* caller uses the builtin diff */
	}

	if (!nr) {
		/*
		 * No hunks reports the blobs equivalent.  A by-oid consumer maps
		 * lines one-to-one, so Git can honor this only for equal counts.
		 * Unequal counts cannot be mapped, so the caller must run builtin diff.
		 */
		free(hunks);
		if (old_lines != new_lines)
			return 0;
		return 1;
	}

	if (validate_external_hunks(hunks, nr, old_lines, new_lines,
				    drv->process, path) < 0) {
		/* validate_external_hunks() warned; drop the process as above. */
		diff_process_mark_failed(drv);
		free(hunks);
		return 0;
	}

	for (i = 0; i < nr; i++) {
		/* xdiff coordinates: 0-based start, count as-is. */
		if (hunk_func(hunks[i].old_start - 1, hunks[i].old_count,
			      hunks[i].new_start - 1, hunks[i].new_count,
			      cb_data) < 0) {
			free(hunks);
			return -1;
		}
	}
	free(hunks);
	return 1;
}
