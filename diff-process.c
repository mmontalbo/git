/*
 * Diff process support: communicate with a long-running external
 * process via the pkt-line protocol to obtain custom line-matching
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
 *   git> capability=hunks / flush
 *   process< capability=hunks / flush
 *
 * Per-file:
 *   git> command=hunks / pathname=<path> / [old-oid=<hex>] / [new-oid=<hex>] / flush
 *   git> <old content packetized> / flush
 *   git> <new content packetized> / flush
 *   process< hunk <old_start> <old_count> <new_start> <new_count>
 *   process< ... / flush
 *   process< status=success / flush
 *
 * When the process returns no hunks with status=success, it considers
 * the files equivalent.  Git will skip the diff for that file.
 *
 * A process that negotiates the "hunks-by-oid" capability (with or
 * without "hunks") may be asked without content, when both sides are
 * stored blobs:
 *   git> command=hunks-by-oid / pathname=<path> / old-oid=<hex> / new-oid=<hex> / flush
 *   process< hunk ... / flush
 *   process< status=success / flush
 * No content sections follow such a request.  The process may instead
 * answer status=need-content, and Git re-asks with a full
 * command=hunks request.  Because Git holds no content for this
 * exchange, the answer is used as sent: the hunks are not re-run
 * through xdiff's compaction, and zero hunks assert equivalence
 * outright (a process that cannot rule out a trailing-newline-only
 * difference from its cache must answer need-content).
 */

#include "git-compat-util.h"
#include "diff-process.h"
#include "diff.h"
#include "diff-provider-internal.h"
#include "gettext.h"
#include "hex.h"
#include "repository.h"
#include "sigchain.h"
#include "userdiff.h"
#include "sub-process.h"
#include "pkt-line.h"
#include "strbuf.h"
#include "xdiff/xdiff.h"

#define CAP_HUNKS (1u << 0)
#define CAP_OID_HUNKS (1u << 1)

struct diff_subprocess {
	struct subprocess_entry subprocess;
	unsigned int supported_capabilities;
};

static int start_diff_process_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = { 1, 0 };
	static struct subprocess_capability capabilities[] = {
		{ "hunks", CAP_HUNKS },
		{ "hunks-by-oid", CAP_OID_HUNKS },
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

	if (drv->diff_subprocess)
		return drv->diff_subprocess;

	entry = xcalloc(1, sizeof(*entry));
	if (subprocess_start_command(&entry->subprocess, drv->process,
				     start_diff_process_fn)) {
		free(entry);
		drv->diff_process_failed = 1;
		warning(_("diff process '%s' failed to start;"
			  " using the builtin diff"), drv->process);
		return NULL;
	}

	drv->diff_subprocess = entry;
	return entry;
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
 * numbering it reports over the protocol.  Kept distinct from struct
 * xdl_hunk (xdiff's coordinates) so that only translated hunks ever
 * reach a consumer; diff_process_hunk_to_xdl() is the single
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
 * Trailing space-separated tokens after the last field are allowed and
 * ignored, so a future protocol version can append fields (e.g. a
 * "moved" marker) without an older Git rejecting the line, mirroring
 * the request-side rule that processes ignore unknown keys.
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
 * Translate a hunk from the diff process's presentation coordinates
 * into xdiff's.
 *
 * Protocol starts are already 1-based positions (the line a change
 * sits before), the same numbering xdiff uses, so the only adjustment
 * is for an empty file side: "git diff" addresses it with a start of 0
 * and a count of 0 (e.g. "0 0 1 5" adds five lines to an empty old
 * side), and since xdiff uses start-1 as an array index that 0 becomes
 * 1 here.  This is NOT the full inverse of xdl_emit_hunk_hdr()
 * (xdiff/xutils.c): that emitter shifts a count-0 range to start-1 for
 * the displayed "@@" header, but the protocol keeps the unshifted
 * 1-based position for a mid-file insert or delete.  This is the single
 * point where presentation coordinates become xdiff coordinates, so
 * xdl_populate_hunks_from_external() may assume 1-based starts.
 *
 * Returns -1 for a start of 0 paired with a nonzero count, which names
 * no line in either coordinate system.  (parse_hunk_line() already
 * guarantees non-negative starts and counts.)
 */
static int diff_process_hunk_to_xdl(const struct diff_process_hunk *presented,
				    struct xdl_hunk *xdl)
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

static enum diff_process_result get_hunks(
		struct userdiff_driver *drv,
		const char *path,
		const char *old_buf, long old_size,
		const char *new_buf, long new_size,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		struct xdl_hunk **hunks_out,
		size_t *nr_hunks_out)
{
	struct diff_subprocess *backend;
	struct child_process *process;
	int fd_in, fd_out;
	struct strbuf status = STRBUF_INIT;
	struct xdl_hunk *hunks = NULL;
	struct diff_process_hunk presented;
	struct xdl_hunk hunk;
	size_t nr_hunks = 0, alloc_hunks = 0;
	size_t max_hunks;
	int len;
	int bad_coords = 0;
	char *line;

	backend = get_or_launch_process(drv);
	if (!backend)
		return DIFF_PROCESS_ERROR;

	if (!(backend->supported_capabilities & CAP_HUNKS))
		return DIFF_PROCESS_SKIP;

	process = subprocess_get_child_process(&backend->subprocess);
	fd_in = process->in;
	fd_out = process->out;

	sigchain_push(SIGPIPE, SIG_IGN);

	/* Send request */
	if (packet_write_fmt_gently(fd_in, "command=hunks\n") ||
	    packet_write_fmt_gently(fd_in, "pathname=%s\n", path))
		goto comm_error;
	/*
	 * old-oid/new-oid let the process key a cache on the blob pair.  A
	 * side is sent only when its content is the raw blob (the caller
	 * passes NULL otherwise, e.g. for textconv'd content), so an oid
	 * that is present always names the bytes the process receives.
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
	 * Hunks are non-overlapping and each useful hunk covers at least
	 * one line, so a valid response cannot contain more hunks than the
	 * two files have lines, which is bounded by their byte sizes.  Cap
	 * the accumulation accordingly so a misbehaving process that floods
	 * hunk lines cannot drive unbounded memory growth before validation.
	 */
	max_hunks = (size_t)old_size + (size_t)new_size + 1;

	/* Read hunks until flush packet */
	while ((len = packet_read_line_gently(fd_out, NULL, &line)) >= 0 &&
	       line) {
		if (parse_hunk_line(line, &presented) < 0)
			goto comm_error;
		if (bad_coords)
			continue;
		if (diff_process_hunk_to_xdl(&presented, &hunk) < 0) {
			/*
			 * Semantically invalid coordinates in a well-formed
			 * response: the stream stays in protocol sync, so
			 * drain the rest and fall back for this file while
			 * keeping the process alive, the same treatment
			 * validate_external_hunks() failures receive.
			 */
			bad_coords = 1;
			continue;
		}
		if (nr_hunks >= max_hunks) {
			warning(_("diff process '%s' sent too many hunks"
				  " for '%s'"), drv->process, path);
			goto comm_error;
		}
		ALLOC_GROW(hunks, nr_hunks + 1, alloc_hunks);
		hunks[nr_hunks++] = hunk;
	}
	if (len < 0)
		goto comm_error;

	/* Read status */
	if (subprocess_read_status(fd_out, &status))
		goto comm_error;

	if (!strcmp(status.buf, "success")) {
		if (bad_coords) {
			warning(_("diff process '%s' returned out-of-range "
				  "coordinates for '%s'; using the builtin diff"),
				drv->process, path);
			free(hunks);
			strbuf_release(&status);
			sigchain_pop(SIGPIPE);
			return DIFF_PROCESS_SKIP;
		}
		*hunks_out = hunks;
		*nr_hunks_out = nr_hunks;
		strbuf_release(&status);
		sigchain_pop(SIGPIPE);
		return DIFF_PROCESS_OK;
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
		return DIFF_PROCESS_SKIP;
	}

	/* status=error or unknown status */
	warning(_("diff process '%s' failed for '%s',"
		  " falling back to builtin diff"),
		drv->process, path);
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_ERROR;

comm_error:
	/*
	 * Communication failure (broken pipe, malformed response).
	 * Tear down the process and mark as failed so we do not
	 * retry on every subsequent file.
	 */
	warning(_("diff process '%s' failed for '%s'; disabling it"
		  " for the remainder of this command"),
		drv->process, path);
	drv->diff_process_failed = 1;
	drv->diff_subprocess = NULL;
	subprocess_stop_command(&backend->subprocess);
	free(backend);
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_ERROR;
}

/*
 * Whether exactly one of the two blobs ends in a newline.  A change
 * that only adds or removes the trailing newline is not expressible as
 * line hunks, so a process comparing lines reports the files as equal.
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
 * Validate the process's hunks (already in xdiff coordinates) before they
 * bypass the diff algorithm.  The content-independent rules (in-order,
 * non-overlapping, lockstep-aligned, int32-bounded coordinates) are the
 * provider interface's shared rule, diff_provider_check_hunk(); this
 * function adds the two checks that need the blobs' line counts (a hunk
 * past the end of a file, the run after the last hunk) and the
 * per-rule diagnostics naming the process.  On a bad response we warn
 * and the caller falls back to the builtin diff.  Returns 0 if valid,
 * -1 (after warning) otherwise.
 *
 * An oid-only answer arrives without content: pass negative line
 * counts, and the two content-dependent checks do not apply.
 */
static int validate_external_hunks(const struct xdl_hunk *hunks, size_t nr,
				   long old_lines, long new_lines,
				   const char *process, const char *path)
{
	struct diff_provider_hunks_check c = { 0 };
	size_t i;

	for (i = 0; i < nr; i++) {
		const struct xdl_hunk *h = &hunks[i];

		if (old_lines >= 0 &&
		    (h->old_count > old_lines - h->old_start + 1 ||
		     h->new_count > new_lines - h->new_start + 1)) {
			warning(_("diff process '%s' returned a hunk past the "
				  "end of '%s'; using the builtin diff"),
				process, path);
			return -1;
		}
		switch (diff_provider_check_hunk(&c, h->old_start,
						  h->old_count, h->new_start,
						  h->new_count)) {
		case PROVIDER_HUNKS_OK:
			break;
		case PROVIDER_HUNKS_RANGE:
			warning(_("diff process '%s' returned out-of-range "
				  "coordinates for '%s'; using the builtin diff"),
				process, path);
			return -1;
		case PROVIDER_HUNKS_OVERLAP:
			warning(_("diff process '%s' returned overlapping hunks "
				  "for '%s'; using the builtin diff"),
				process, path);
			return -1;
		case PROVIDER_HUNKS_MISALIGNED:
			warning(_("diff process '%s' returned hunks that leave "
				  "'%s' misaligned; using the builtin diff"),
				process, path);
			return -1;
		}
	}
	if (old_lines >= 0 &&
	    old_lines - c.prev_old_end != new_lines - c.prev_new_end) {
		warning(_("diff process '%s' returned hunks that leave '%s' "
			  "misaligned; using the builtin diff"),
			process, path);
		return -1;
	}
	return 0;
}

struct userdiff_driver *diff_process_driver(struct diff_options *diffopt,
					    const char *path,
					    const xpparam_t *xpp)
{
	struct userdiff_driver *drv;

	if (!diffopt || !path)
		return NULL;
	if (!diffopt->flags.allow_diff_process || diffopt->ignore_driver_algorithm)
		return NULL;
	/*
	 * Whitespace-ignoring, regex-ignore (-I) and anchored options
	 * change which lines count as different, but the process is never
	 * told about them, so its hunks could not honor them.  Rather
	 * than silently override the user's request, fall back to the
	 * builtin diff, which does honor these flags.  Key this off xpp
	 * (the parameters this diff actually runs with) rather than
	 * diffopt, so a caller like blame that keeps its flags outside
	 * diffopt is covered without a separate guard of its own.
	 */
	if ((xpp->flags & (XDF_WHITESPACE_FLAGS | XDF_IGNORE_BLANK_LINES)) ||
	    xpp->ignore_regex_nr || xpp->anchors_nr)
		return NULL;

	drv = userdiff_find_by_path(diffopt->repo->index, path);
	if (!drv || !drv->process)
		return NULL;
	if (drv->diff_process_failed)
		return NULL;
	return drv;
}

enum diff_process_result diff_process_fill_hunks(
		struct diff_options *diffopt,
		const char *path,
		const mmfile_t *file_a,
		const mmfile_t *file_b,
		const struct object_id *oid_a,
		const struct object_id *oid_b,
		xpparam_t *xpp)
{
	struct userdiff_driver *drv;
	struct xdl_hunk *ext_hunks = NULL;
	size_t nr = 0;
	enum diff_process_result res;

	drv = diff_process_driver(diffopt, path, xpp);
	if (!drv)
		return DIFF_PROCESS_SKIP;

	/*
	 * The protocol reports line hunks, which binary content does not
	 * have, so binary files are never sent to the process.
	 */
	if (buffer_is_binary(file_a->ptr, file_a->size) ||
	    buffer_is_binary(file_b->ptr, file_b->size))
		return DIFF_PROCESS_SKIP;

	res = get_hunks(drv, path,
			file_a->ptr, file_a->size,
			file_b->ptr, file_b->size,
			oid_a, oid_b,
			&ext_hunks, &nr);
	if (res == DIFF_PROCESS_OK) {
		if (!nr) {
			free(ext_hunks);
			/*
			 * Zero hunks means the process considers the line
			 * content identical, but it cannot express a
			 * trailing-newline-only change.  When that is the
			 * actual difference, fall back to the builtin diff
			 * so the "\ No newline at end of file" marker is
			 * preserved instead of reporting the files equal.
			 */
			if (eof_newline_differs(file_a, file_b))
				return DIFF_PROCESS_SKIP;
			return DIFF_PROCESS_EQUIVALENT;
		}
		if (validate_external_hunks(ext_hunks, nr,
					    count_lines(file_a->ptr, file_a->size),
					    count_lines(file_b->ptr, file_b->size),
					    drv->process, path) < 0) {
			free(ext_hunks);
			return DIFF_PROCESS_SKIP;
		}
		xpp->external_hunks = ext_hunks;
		xpp->external_hunks_nr = nr;
		return DIFF_PROCESS_OK;
	}
	/*
	 * get_hunks() warns where a warning is due; SKIP may also be a
	 * silent stand-down (missing capability, status=abort).
	 */
	return res;
}

/*
 * Without content there is no size-derived bound on a response, so cap
 * accumulation at a constant instead.  A response that exceeds the
 * cap is a protocol error: the process is disabled for the rest of
 * the command and the caller falls back to the builtin diff.
 */
#define OID_HUNKS_MAX (1 << 20)

enum diff_process_result diff_process_query_hunks(
		struct diff_options *diffopt,
		const char *path,
		const struct object_id *old_oid,
		const struct object_id *new_oid,
		const xpparam_t *xpp,
		xdl_emit_hunk_consume_func_t hunk_cb,
		void *cb_data)
{
	struct userdiff_driver *drv;
	struct diff_subprocess *backend;
	struct child_process *process;
	int fd_in, fd_out;
	struct strbuf status = STRBUF_INIT;
	struct xdl_hunk *hunks = NULL;
	struct diff_process_hunk presented;
	struct xdl_hunk hunk;
	size_t nr_hunks = 0, alloc_hunks = 0, i;
	int len;
	int bad_coords = 0;
	char *line;
	enum diff_process_result res;

	if (!old_oid || !new_oid)
		return DIFF_PROCESS_SKIP;
	drv = diff_process_driver(diffopt, path, xpp);
	if (!drv)
		return DIFF_PROCESS_SKIP;

	backend = get_or_launch_process(drv);
	if (!backend)
		return DIFF_PROCESS_ERROR;
	/*
	 * hunks-by-oid stands alone: a process that only fetches by object
	 * id (a cache front-end) need not accept content, so CAP_HUNKS
	 * is not required here.  When such a process answers need-content,
	 * the caller's content request finds CAP_HUNKS missing and the
	 * builtin diff runs.
	 */
	if (!(backend->supported_capabilities & CAP_OID_HUNKS))
		return DIFF_PROCESS_SKIP;

	process = subprocess_get_child_process(&backend->subprocess);
	fd_in = process->in;
	fd_out = process->out;

	sigchain_push(SIGPIPE, SIG_IGN);

	if (packet_write_fmt_gently(fd_in, "command=hunks-by-oid\n") ||
	    packet_write_fmt_gently(fd_in, "pathname=%s\n", path) ||
	    packet_write_fmt_gently(fd_in, "old-oid=%s\n", oid_to_hex(old_oid)) ||
	    packet_write_fmt_gently(fd_in, "new-oid=%s\n", oid_to_hex(new_oid)) ||
	    packet_flush_gently(fd_in))
		goto comm_error;

	while ((len = packet_read_line_gently(fd_out, NULL, &line)) >= 0 &&
	       line) {
		if (parse_hunk_line(line, &presented) < 0)
			goto comm_error;
		if (bad_coords)
			continue;
		if (diff_process_hunk_to_xdl(&presented, &hunk) < 0) {
			/*
			 * Semantically invalid coordinates in a well-formed
			 * response: the stream stays in protocol sync, so
			 * drain the rest and fall back for this file while
			 * keeping the process alive, the same treatment
			 * validate_external_hunks() failures receive.
			 */
			bad_coords = 1;
			continue;
		}
		if (nr_hunks >= OID_HUNKS_MAX) {
			warning(_("diff process '%s' sent too many hunks"
				  " for '%s'"), drv->process, path);
			goto comm_error;
		}
		ALLOC_GROW(hunks, nr_hunks + 1, alloc_hunks);
		hunks[nr_hunks++] = hunk;
	}
	if (len < 0)
		goto comm_error;

	if (subprocess_read_status(fd_out, &status))
		goto comm_error;

	if (!strcmp(status.buf, "success")) {
		if (bad_coords) {
			warning(_("diff process '%s' returned out-of-range "
				  "coordinates for '%s'; using the builtin diff"),
				drv->process, path);
			res = DIFF_PROCESS_SKIP;
			goto out;
		}
		if (validate_external_hunks(hunks, nr_hunks, -1, -1,
					    drv->process, path) < 0) {
			res = DIFF_PROCESS_SKIP;
			goto out;
		}
		if (!nr_hunks) {
			res = DIFF_PROCESS_EQUIVALENT;
			goto out;
		}
		/*
		 * Replay in the coordinates a hunk consumer receives from
		 * xdiff's emission: 0-based starts.  The answer is used as
		 * the process sent it; with no content in hand it cannot be
		 * re-run through xdiff's compaction.
		 */
		for (i = 0; i < nr_hunks; i++)
			hunk_cb(hunks[i].old_start - 1, hunks[i].old_count,
				hunks[i].new_start - 1, hunks[i].new_count,
				cb_data);
		res = DIFF_PROCESS_OK;
		goto out;
	}
	if (!strcmp(status.buf, "need-content")) {
		/* The process wants the content request; the caller sends it. */
		res = DIFF_PROCESS_SKIP;
		goto out;
	}
	if (!strcmp(status.buf, "abort")) {
		/*
		 * The process withdrew from oid-only answers: stop asking, but
		 * keep consulting it with content.
		 */
		backend->supported_capabilities &= ~CAP_OID_HUNKS;
		res = DIFF_PROCESS_SKIP;
		goto out;
	}
	warning(_("diff process '%s' failed for '%s',"
		  " falling back to builtin diff"),
		drv->process, path);
	res = DIFF_PROCESS_ERROR;
out:
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return res;

comm_error:
	warning(_("diff process '%s' failed for '%s'; disabling it"
		  " for the remainder of this command"),
		drv->process, path);
	drv->diff_process_failed = 1;
	drv->diff_subprocess = NULL;
	subprocess_stop_command(&backend->subprocess);
	free(backend);
	free(hunks);
	strbuf_release(&status);
	sigchain_pop(SIGPIPE);
	return DIFF_PROCESS_ERROR;
}
