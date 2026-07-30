/*
 * Diff process support: consult a long-running external process via the
 * pkt-line protocol for the hunks of a blob pair.  The process answers
 * from the pair's object names alone: it can serve a persistent cache
 * keyed on the pair, or fetch the blobs from the repository itself
 * (e.g. via "git cat-file --batch") and compute its own notion of
 * which lines changed.
 *
 * Protocol: pkt-line over stdin/stdout, following the pattern of
 * the long-running filter process protocol (see convert.c).
 *
 * Handshake:
 *   git> git-diff-client / version=1 / flush
 *   process< git-diff-server / version=1 / flush
 *   git> capability=hunks-by-oid / flush
 *   process< capability=hunks-by-oid / flush
 *
 * Per-pair, when both sides are stored blobs:
 *   git> command=hunks-by-oid / pathname=<path> / old-oid=<hex> / new-oid=<hex> / flush
 *   process< hunk <old_start> <old_count> <new_start> <new_count>
 *   process< ... / flush
 *   process< status=success / flush
 *
 * No content is sent.  Because Git holds no content for the exchange,
 * the answer is used as the process sent it: the hunks are not re-run
 * through xdiff's compaction, and a status=success response with zero
 * hunks asserts that the blobs are equivalent, including their
 * trailing newlines.  A process that cannot answer from the object names
 * (or cannot rule out a trailing-newline-only difference) responds
 * status=need-content; Git then computes the pair itself.  A later
 * protocol extension can define a content-carrying request for such
 * processes and for sides that are not stored blobs.
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

#define CAP_OID_HUNKS (1u << 0)

struct diff_subprocess {
	struct subprocess_entry subprocess;
	unsigned int supported_capabilities;
};

static int start_diff_process_fn(struct subprocess_entry *subprocess)
{
	static int versions[] = { 1, 0 };
	static struct subprocess_capability capabilities[] = {
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
 * any consumer of these coordinates may assume 1-based starts.
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
		/*
		 * The process cannot answer this pair from its object names;
		 * the caller computes the diff itself.
		 */
		res = DIFF_PROCESS_SKIP;
		goto out;
	}
	if (!strcmp(status.buf, "abort")) {
		/* The process withdrew: stop asking it for this session. */
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
