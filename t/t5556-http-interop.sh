#!/bin/sh

test_description='smart-HTTP clone/fetch/push against different web servers

This is a small parity smoke test for the lib-httpd.sh web-server backends.
It drives clone, push and fetch over the smart-HTTP protocol against whichever
server LIB_HTTPD_TYPE selects, so an alternate backend can be exercised with

	GIT_TEST_HTTPD=true LIB_HTTPD_TYPE=lighttpd ./t5556-http-interop.sh

The default backend remains Apache, matching the rest of the t55xx tests.
'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

TEST_NO_CREATE_REPO=t

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-httpd.sh
start_httpd
setup_askpass_helper

test_expect_success 'setup server repository' '
	git init --bare "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" config http.receivepack true &&

	git init work &&
	test_commit -C work one &&
	git -C work push "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" HEAD:main
'

test_expect_success 'clone over smart HTTP' '
	git clone "$HTTPD_URL/smart/repo.git" clone1 &&
	git -C work rev-parse main >expect &&
	git -C clone1 rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'push over smart HTTP' '
	test_commit -C clone1 two &&
	git -C clone1 push origin main &&
	git -C clone1 rev-parse main >expect &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'fetch new history over smart HTTP' '
	git clone "$HTTPD_URL/smart/repo.git" clone2 &&
	test_commit -C clone2 three &&
	git -C clone2 push origin main &&
	git -C clone1 fetch origin &&
	git -C clone2 rev-parse main >expect &&
	git -C clone1 rev-parse origin/main >actual &&
	test_cmp expect actual
'

test_expect_success 'smart HTTP negotiates protocol v2' '
	GIT_TRACE_PACKET=1 git -c protocol.version=2 \
		ls-remote "$HTTPD_URL/smart/repo.git" >/dev/null 2>trace &&
	grep "git< version 2" trace
'

test_expect_success 'clone over authenticated smart HTTP' '
	set_askpass user@host pass@host &&
	git clone "$HTTPD_URL/auth/smart/repo.git" clone-auth &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >expect &&
	git -C clone-auth rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'authenticated smart HTTP rejects bad credentials' '
	set_askpass wrong-user wrong-pass &&
	test_must_fail git clone "$HTTPD_URL/auth/smart/repo.git" clone-auth-fail
'

test_expect_success 'clone follows a permanent redirect' '
	git clone "$HTTPD_URL/smart-redir-perm/repo.git" clone-redir &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >expect &&
	git -C clone-redir rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'clone follows a temporary redirect' '
	git clone "$HTTPD_URL/smart-redir-temp/repo.git" clone-redir-temp &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >expect &&
	git -C clone-redir-temp rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'clone over dumb HTTP' '
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" update-server-info &&
	git clone "$HTTPD_URL/dumb/repo.git" clone-dumb &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >expect &&
	git -C clone-dumb rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'anonymous fetch from auth-push repo' '
	set_askpass none none &&
	git clone "$HTTPD_URL/auth-push/smart/repo.git" clone-anon &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >expect &&
	git -C clone-anon rev-parse main >actual &&
	test_cmp expect actual
'

test_expect_success 'push to auth-push repo requires credentials' '
	test_commit -C clone-anon four &&
	set_askpass user@host pass@host &&
	git -C clone-anon push origin main &&
	git -C clone-anon rev-parse main >expect &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/repo.git" rev-parse main >actual &&
	test_cmp expect actual
'

# Cover git's large-body push/fetch path with a pack bigger than a stock
# reverse-proxy request-body limit. This is transport coverage, not a git
# fix: the server just has to be configured to accept the body (nginx.conf
# raises client_max_body_size), which is a fixture requirement.
test_expect_success 'large blob roundtrips over smart HTTP' '
	git init --bare "$HTTPD_DOCUMENT_ROOT_PATH/big.git" &&
	git -C "$HTTPD_DOCUMENT_ROOT_PATH/big.git" config http.receivepack true &&

	git init big-work &&
	test-tool genrandom seed 2000000 >big-work/big.bin &&
	git -C big-work add big.bin &&
	git -C big-work commit -q -m big &&

	git -C big-work push "$HTTPD_URL/smart/big.git" HEAD:main &&
	git clone "$HTTPD_URL/smart/big.git" big-clone &&
	git -C big-clone fsck &&

	git -C big-work rev-parse HEAD:big.bin >expect &&
	git -C big-clone rev-parse HEAD:big.bin >actual &&
	test_cmp expect actual
'

test_done
