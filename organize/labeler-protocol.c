#include "git-compat-util.h"
#include "labeler-protocol.h"
#include "gettext.h"
#include "run-command.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "wrapper.h"

/*
 * Run the labeler and record, in `labeled` (path -> its "k=value k=value" string in
 * util), each scoped_files file it labels. The labeler writes `path \0 labels \0`
 * per file, where labels is its space-separated key=value labels.
 */
void run_labeler(const char *cmd, struct string_list *scoped_files,
		 struct string_list *labeled)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	const char *p, *end;

	strvec_push(&cp.args, cmd);
	cp.use_shell = 1;
	if (capture_command(&cp, &out, 0))
		die(_("organize apply --labels-only: labeler failed: %s"), cmd);

	p = out.buf;
	end = out.buf + out.len;
	while (p < end) {
		const char *path = p;
		const char *labels;

		p += strlen(p) + 1;
		if (p > end)
			die(_("organize apply --labels-only: truncated labeler record"));
		labels = p;
		p += strlen(p) + 1;
		if (p > end)
			die(_("organize apply --labels-only: truncated labeler record"));

		if (!string_list_has_string(scoped_files, path))
			continue;	/* not a scoped_files file */
		string_list_insert(labeled, path)->util = xstrdup(labels);
	}
	strbuf_release(&out);
}
