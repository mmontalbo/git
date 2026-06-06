#include "git-compat-util.h"
#include "trace2.h"

static void check_bug_if_BUG(void)
{
	if (!bug_called_must_BUG)
		return;
	BUG("on exit(): had bug() call(s) in this process without explicit BUG_if_bug()");
}

static common_exit_intercept_fn exit_intercept;

void set_exit_intercept(common_exit_intercept_fn fn)
{
	exit_intercept = fn;
}

/* We wrap exit() to call common_exit() in git-compat-util.h */
int common_exit(const char *file, int line, int code)
{
	/*
	 * For non-POSIX systems: Take the lowest 8 bits of the "code"
	 * to e.g. turn -1 into 255. On a POSIX system this is
	 * redundant, see exit(3) and wait(2), but as it doesn't harm
	 * anything there we don't need to guard this with an "ifdef".
	 */
	code &= 0xff;

	/*
	 * If an intercept is registered (e.g. by git-batch), call it.
	 * The intercept is expected to not return (e.g. siglongjmp).
	 * This runs before any atexit handlers or process teardown,
	 * so there is no undefined behavior.
	 */
	if (exit_intercept)
		exit_intercept(code);

	check_bug_if_BUG();
	trace2_cmd_exit_fl(file, line, code);

	return code;
}
