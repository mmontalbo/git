# Shell library to run an HTTP server for use in tests.
# Ends the test early if httpd tests should not be run,
# for example because the user has not enabled them.
#
# Usage:
#
#	. ./test-lib.sh
#	. "$TEST_DIRECTORY"/lib-httpd.sh
#	start_httpd
#
#	test_expect_success '...' '
#		...
#	'
#
#	test_expect_success ...
#
#	test_done
#
# Can be configured using the following variables.
#
#    GIT_TEST_HTTPD              enable HTTPD tests
#    LIB_HTTPD_PATH              web server path
#    LIB_HTTPD_MODULE_PATH       web server modules path
#    LIB_HTTPD_PORT              listening port
#    LIB_HTTPD_DAV               enable DAV
#    LIB_HTTPD_SVN               enable SVN at given location (e.g. "svn")
#    LIB_HTTPD_SSL               enable SSL
#    LIB_HTTPD_PROXY             enable proxy
#
# Copyright (c) 2008 Clemens Buchacher <drizzd@aon.at>
#

if ! test_have_prereq LIBCURL
then
	skip_all='skipping test, git built without http support'
	test_done
fi

if test -n "$NO_EXPAT" && test -n "$LIB_HTTPD_DAV"
then
	skip_all='skipping test, git built without expat support'
	test_done
fi

if ! test_bool_env GIT_TEST_HTTPD true
then
	skip_all="Network testing disabled (unset GIT_TEST_HTTPD to enable)"
	test_done
fi

if ! test_have_prereq NOT_ROOT; then
	test_skip_or_die GIT_TEST_HTTPD \
		"Cannot run httpd tests as root"
fi

HTTPD_PARA=""

# Which web server backend to drive. "apache" preserves the historical
# behavior; "lighttpd" runs the plain smart/dumb HTTP subset (see
# lib-httpd/lighttpd.conf). Each backend defines lib_httpd_start and
# lib_httpd_stop and sets HTTPD_CONFIG below.
LIB_HTTPD_TYPE="${LIB_HTTPD_TYPE:-apache}"

test_set_port LIB_HTTPD_PORT

TEST_PATH="$TEST_DIRECTORY"/lib-httpd
HTTPD_ROOT_PATH="$PWD"/httpd
HTTPD_DOCUMENT_ROOT_PATH=$HTTPD_ROOT_PATH/www

# hack to suppress apache PassEnv warnings
GIT_VALGRIND=$GIT_VALGRIND; export GIT_VALGRIND
GIT_VALGRIND_OPTIONS=$GIT_VALGRIND_OPTIONS; export GIT_VALGRIND_OPTIONS
GIT_TEST_SIDEBAND_ALL=$GIT_TEST_SIDEBAND_ALL; export GIT_TEST_SIDEBAND_ALL
GIT_TRACE=$GIT_TRACE; export GIT_TRACE

# Locate Apache, verify its version and pick a module directory, then
# define the start/stop hooks for the Apache backend.
lib_httpd_setup_apache () {
	for DEFAULT_HTTPD_PATH in '/usr/sbin/httpd' \
				  '/usr/sbin/apache2' \
				  "$(command -v httpd)" \
				  "$(command -v apache2)"
	do
		if test -n "$DEFAULT_HTTPD_PATH" && test -x "$DEFAULT_HTTPD_PATH"
		then
			break
		fi
	done

	if test -x "$DEFAULT_HTTPD_PATH"
	then
		DETECTED_HTTPD_ROOT="$("$DEFAULT_HTTPD_PATH" -V 2>/dev/null | sed -n 's/^ -D HTTPD_ROOT="\(.*\)"$/\1/p')"
	fi

	for DEFAULT_HTTPD_MODULE_PATH in '/usr/libexec/apache2' \
					 '/usr/lib/apache2/modules' \
					 '/usr/lib64/httpd/modules' \
					 '/usr/lib/httpd/modules' \
					 '/usr/libexec/httpd' \
					 '/usr/lib/apache2' \
					 "${DETECTED_HTTPD_ROOT:+${DETECTED_HTTPD_ROOT}/modules}"
	do
		if test -n "$DEFAULT_HTTPD_MODULE_PATH" && test -d "$DEFAULT_HTTPD_MODULE_PATH"
		then
			break
		fi
	done

	case $(uname) in
		Darwin)
			HTTPD_PARA="$HTTPD_PARA -DDarwin"
		;;
	esac

	LIB_HTTPD_PATH=${LIB_HTTPD_PATH-"$DEFAULT_HTTPD_PATH"}

	if ! test -x "$LIB_HTTPD_PATH"
	then
		test_skip_or_die GIT_TEST_HTTPD "no web server found at '$LIB_HTTPD_PATH'"
	fi

	HTTPD_VERSION=$($LIB_HTTPD_PATH -v | \
		sed -n 's/^Server version: Apache\/\([0-9.]*\).*$/\1/p; q')
	HTTPD_VERSION_MAJOR=$(echo $HTTPD_VERSION | cut -d. -f1)
	HTTPD_VERSION_MINOR=$(echo $HTTPD_VERSION | cut -d. -f2)

	if test -n "$HTTPD_VERSION_MAJOR"
	then
		if test -z "$LIB_HTTPD_MODULE_PATH"
		then
			if ! test "$HTTPD_VERSION_MAJOR" -eq 2 ||
			   ! test "$HTTPD_VERSION_MINOR" -ge 4
			then
				test_skip_or_die GIT_TEST_HTTPD \
					"at least Apache version 2.4 is required"
			fi
			if ! test -d "$DEFAULT_HTTPD_MODULE_PATH"
			then
				test_skip_or_die GIT_TEST_HTTPD \
					"Apache module directory not found"
			fi

			LIB_HTTPD_MODULE_PATH="$DEFAULT_HTTPD_MODULE_PATH"
		fi
	else
		test_skip_or_die GIT_TEST_HTTPD \
			"Could not identify web server at '$LIB_HTTPD_PATH'"
	fi

	if test -n "$LIB_HTTPD_DAV" && test -f /etc/os-release
	then
		case "$(grep "^ID=" /etc/os-release | cut -d= -f2-)" in
		alpine)
			# The WebDAV module in Alpine Linux is broken at least up to
			# Alpine v3.16 as the default DBM driver is missing.
			#
			# https://gitlab.alpinelinux.org/alpine/aports/-/issues/13112
			test_skip_or_die GIT_TEST_HTTPD \
				"Apache WebDAV module does not have default DBM backend driver"
			;;
		esac
	fi

	HTTPD_CONFIG="$TEST_PATH/apache.conf"

	lib_httpd_start () {
		"$LIB_HTTPD_PATH" -d "$HTTPD_ROOT_PATH" \
			-f "$HTTPD_CONFIG" $HTTPD_PARA \
			-c "Listen 127.0.0.1:$LIB_HTTPD_PORT" -k start
	}

	lib_httpd_stop () {
		"$LIB_HTTPD_PATH" -d "$HTTPD_ROOT_PATH" \
			-f "$HTTPD_CONFIG" $HTTPD_PARA -k stop
	}
}

# lighttpd counterpart to lib_httpd_setup_apache. Supports only the plain
# smart/dumb HTTP subset; prepare_httpd() rejects SSL/DAV/SVN/proxy here.
lib_httpd_setup_lighttpd () {
	for DEFAULT_HTTPD_PATH in '/usr/sbin/lighttpd' \
				  "$(command -v lighttpd)"
	do
		if test -n "$DEFAULT_HTTPD_PATH" && test -x "$DEFAULT_HTTPD_PATH"
		then
			break
		fi
	done

	LIB_HTTPD_PATH=${LIB_HTTPD_PATH-"$DEFAULT_HTTPD_PATH"}

	if ! test -x "$LIB_HTTPD_PATH"
	then
		test_skip_or_die GIT_TEST_HTTPD "no web server found at '$LIB_HTTPD_PATH'"
	fi

	HTTPD_VERSION=$("$LIB_HTTPD_PATH" -v 2>/dev/null | \
		sed -n 's|^lighttpd/\([0-9.]*\).*|\1|p')
	case "$HTTPD_VERSION" in
	1.4.*)
		;;
	*)
		test_skip_or_die GIT_TEST_HTTPD \
			"lighttpd 1.4.x is required (found '${HTTPD_VERSION:-unknown}')"
		;;
	esac

	HTTPD_CONFIG="$TEST_PATH/lighttpd.conf"

	# lighttpd has no -k start/stop; it daemonizes and records its pid in
	# the file named by server.pid-file in lighttpd.conf. Dynamic values
	# are handed to it through the environment.
	lib_httpd_start () {
		# lighttpd.conf has no if/else, so generate the TLS + HTTP/2
		# snippet it includes (empty unless SSL was requested). lighttpd
		# offers h2 over TLS via ALPN by default.
		if test -n "$LIB_HTTPD_SSL"
		then
			cat >"$HTTPD_ROOT_PATH/lighttpd-ssl.conf" <<-EOF
			server.modules += ( "mod_openssl" )
			ssl.engine  = "enable"
			ssl.pemfile = "$HTTPD_ROOT_PATH/httpd.pem"
			EOF
		else
			: >"$HTTPD_ROOT_PATH/lighttpd-ssl.conf"
		fi

		LIB_HTTPD_PORT="$LIB_HTTPD_PORT" \
		HTTPD_ROOT_PATH="$HTTPD_ROOT_PATH" \
		HTTPD_DOCUMENT_ROOT_PATH="$HTTPD_DOCUMENT_ROOT_PATH" \
		GIT_EXEC_PATH="$GIT_EXEC_PATH" \
			"$LIB_HTTPD_PATH" -f "$HTTPD_CONFIG"
	}

	lib_httpd_stop () {
		test -f "$HTTPD_ROOT_PATH/httpd.pid" &&
		kill "$(cat "$HTTPD_ROOT_PATH/httpd.pid")"
	}
}

# nginx counterpart. nginx has no native CGI, so git-http-backend is driven
# through an fcgiwrap daemon that we start alongside it and reach over a Unix
# socket. Like lighttpd, only the plain smart/dumb HTTP subset is supported.
lib_httpd_setup_nginx () {
	for DEFAULT_HTTPD_PATH in '/usr/sbin/nginx' \
				  "$(command -v nginx)"
	do
		if test -n "$DEFAULT_HTTPD_PATH" && test -x "$DEFAULT_HTTPD_PATH"
		then
			break
		fi
	done

	LIB_HTTPD_PATH=${LIB_HTTPD_PATH-"$DEFAULT_HTTPD_PATH"}

	if ! test -x "$LIB_HTTPD_PATH"
	then
		test_skip_or_die GIT_TEST_HTTPD "no web server found at '$LIB_HTTPD_PATH'"
	fi

	LIB_HTTPD_FCGIWRAP_PATH="${LIB_HTTPD_FCGIWRAP_PATH-$(command -v fcgiwrap)}"
	if ! test -x "$LIB_HTTPD_FCGIWRAP_PATH"
	then
		test_skip_or_die GIT_TEST_HTTPD \
			"the nginx backend requires fcgiwrap (set LIB_HTTPD_FCGIWRAP_PATH)"
	fi

	HTTPD_VERSION=$("$LIB_HTTPD_PATH" -v 2>&1 | \
		sed -n 's|^nginx version: nginx/\([0-9.]*\).*|\1|p')

	# nginx has no config-time environment interpolation, so the concrete
	# config is generated from the nginx.conf template at start time.
	HTTPD_CONFIG="$HTTPD_ROOT_PATH/nginx.conf"

	lib_httpd_start () {
		fcgiwrap_socket="$HTTPD_ROOT_PATH/fcgiwrap.sock"

		# nginx's TLS listener also advertises HTTP/2 over ALPN, so the
		# same config exercises HTTPS and HTTP/2 when LIB_HTTPD_SSL is set.
		if test -n "$LIB_HTTPD_SSL"
		then
			nginx_listen="listen 127.0.0.1:$LIB_HTTPD_PORT ssl;"
			nginx_ssl_params="http2 on; ssl_certificate \"$HTTPD_ROOT_PATH/httpd.pem\"; ssl_certificate_key \"$HTTPD_ROOT_PATH/httpd.pem\";"
		else
			nginx_listen="listen 127.0.0.1:$LIB_HTTPD_PORT;"
			nginx_ssl_params=""
		fi

		sed -e "s|@LIB_HTTPD_PORT@|$LIB_HTTPD_PORT|g" \
		    -e "s|@HTTPD_ROOT_PATH@|$HTTPD_ROOT_PATH|g" \
		    -e "s|@HTTPD_DOCUMENT_ROOT_PATH@|$HTTPD_DOCUMENT_ROOT_PATH|g" \
		    -e "s|@GIT_EXEC_PATH@|$GIT_EXEC_PATH|g" \
		    -e "s|@LISTEN@|$nginx_listen|g" \
		    -e "s|@SSL_PARAMS@|$nginx_ssl_params|g" \
		    "$TEST_PATH/nginx.conf" >"$HTTPD_CONFIG" || return 1

		# Start the CGI shim first, then nginx in front of it.
		rm -f "$fcgiwrap_socket"
		"$LIB_HTTPD_FCGIWRAP_PATH" -c 1 -s "unix:$fcgiwrap_socket" &
		echo $! >"$HTTPD_ROOT_PATH/fcgiwrap.pid"

		# -e overrides the compile-time default error log, which nginx
		# opens before it has parsed the config's error_log directive.
		"$LIB_HTTPD_PATH" -p "$HTTPD_ROOT_PATH" \
			-e "$HTTPD_ROOT_PATH/error.log" -c "$HTTPD_CONFIG"
	}

	lib_httpd_stop () {
		if test -f "$HTTPD_ROOT_PATH/httpd.pid"
		then
			kill "$(cat "$HTTPD_ROOT_PATH/httpd.pid")" 2>/dev/null
		fi
		if test -f "$HTTPD_ROOT_PATH/fcgiwrap.pid"
		then
			kill "$(cat "$HTTPD_ROOT_PATH/fcgiwrap.pid")" 2>/dev/null
		fi
		return 0
	}
}

case "$LIB_HTTPD_TYPE" in
apache)
	lib_httpd_setup_apache
	;;
lighttpd)
	lib_httpd_setup_lighttpd
	;;
nginx)
	lib_httpd_setup_nginx
	;;
*)
	test_skip_or_die GIT_TEST_HTTPD \
		"unknown web server type '$LIB_HTTPD_TYPE'"
	;;
esac

install_script () {
	write_script "$HTTPD_ROOT_PATH/$1" <"$TEST_PATH/$1"
}

prepare_httpd() {
	mkdir -p "$HTTPD_DOCUMENT_ROOT_PATH"
	cp "$TEST_PATH"/passwd "$HTTPD_ROOT_PATH"
	cp "$TEST_PATH"/proxy-passwd "$HTTPD_ROOT_PATH"
	install_script incomplete-length-upload-pack-v2-http.sh
	install_script incomplete-body-upload-pack-v2-http.sh
	install_script error-no-report.sh
	install_script broken-smart-http.sh
	install_script error-smart-http.sh
	install_script error.sh
	install_script apply-one-time-script.sh
	install_script nph-custom-auth.sh
	install_script http-429.sh

	case "$LIB_HTTPD_TYPE" in
	apache)
		ln -s "$LIB_HTTPD_MODULE_PATH" "$HTTPD_ROOT_PATH/modules"
		;;
	lighttpd)
		# The lighttpd config adds TLS and HTTP/2, but no WebDAV,
		# Subversion or forward-proxy support.
		if test -n "$LIB_HTTPD_DAV$LIB_HTTPD_SVN$LIB_HTTPD_PROXY"
		then
			test_skip_or_die GIT_TEST_HTTPD \
				"the lighttpd backend supports smart/dumb HTTP(S) only"
		fi
		;;
	nginx)
		# The nginx config adds TLS and HTTP/2, but still no WebDAV,
		# Subversion or forward-proxy support.
		if test -n "$LIB_HTTPD_DAV$LIB_HTTPD_SVN$LIB_HTTPD_PROXY"
		then
			test_skip_or_die GIT_TEST_HTTPD \
				"the nginx backend supports smart/dumb HTTP(S) only"
		fi
		;;
	esac

	if test -n "$LIB_HTTPD_SSL"
	then
		HTTPD_PROTO=https

		RANDFILE_PATH="$HTTPD_ROOT_PATH"/.rnd openssl req \
			-config "$TEST_PATH/ssl.cnf" \
			-new -x509 -nodes \
			-out "$HTTPD_ROOT_PATH/httpd.pem" \
			-keyout "$HTTPD_ROOT_PATH/httpd.pem"
		GIT_SSL_NO_VERIFY=t
		export GIT_SSL_NO_VERIFY
		HTTPD_PARA="$HTTPD_PARA -DSSL"
	else
		HTTPD_PROTO=http
	fi
	HTTPD_DEST=127.0.0.1:$LIB_HTTPD_PORT
	HTTPD_URL=$HTTPD_PROTO://$HTTPD_DEST
	HTTPD_URL_USER=$HTTPD_PROTO://user%40host@$HTTPD_DEST
	HTTPD_URL_USER_PASS=$HTTPD_PROTO://user%40host:pass%40host@$HTTPD_DEST

	if test -n "$LIB_HTTPD_DAV" || test -n "$LIB_HTTPD_SVN"
	then
		HTTPD_PARA="$HTTPD_PARA -DDAV"

		if test -n "$LIB_HTTPD_SVN"
		then
			HTTPD_PARA="$HTTPD_PARA -DSVN"
			LIB_HTTPD_SVNPATH="$rawsvnrepo"
			svnrepo="http://127.0.0.1:$LIB_HTTPD_PORT/"
			svnrepo="$svnrepo$LIB_HTTPD_SVN"
			export LIB_HTTPD_SVN LIB_HTTPD_SVNPATH
		fi
	fi

	if test -n "$LIB_HTTPD_PROXY"
	then
		HTTPD_PARA="$HTTPD_PARA -DPROXY"
	fi
}

enable_http2 () {
	HTTPD_PARA="$HTTPD_PARA -DHTTP2"
	test_set_prereq HTTP2
}

enable_cgipassauth () {
	# We are looking for 2.4.13 or more recent. Since we only support
	# 2.4 and up, no need to check for older major/minor.
	if test "$HTTPD_VERSION_MAJOR" = 2 &&
	   test "$HTTPD_VERSION_MINOR" = 4 &&
	   test "$(echo $HTTPD_VERSION | cut -d. -f3)" -lt 13
	then
		echo >&4 "apache $HTTPD_VERSION too old for CGIPassAuth"
		return
	fi
	HTTPD_PARA="$HTTPD_PARA -DUSE_CGIPASSAUTH"
	test_set_prereq CGIPASSAUTH
}

start_httpd() {
	prepare_httpd >&3 2>&4

	test_atexit stop_httpd

	if ! lib_httpd_start >&3 2>&4
	then
		cat "$HTTPD_ROOT_PATH"/error.log >&4 2>/dev/null
		test_skip_or_die GIT_TEST_HTTPD "web server setup failed"
	fi
}

stop_httpd() {
	lib_httpd_stop
}

test_http_push_nonff () {
	REMOTE_REPO=$1
	LOCAL_REPO=$2
	BRANCH=$3
	EXPECT_CAS_RESULT=${4-failure}

	test_expect_success 'non-fast-forward push fails' '
		cd "$REMOTE_REPO" &&
		HEAD=$(git rev-parse --verify HEAD) &&

		cd "$LOCAL_REPO" &&
		git checkout $BRANCH &&
		echo "changed" > path2 &&
		git commit -a -m path2 --amend &&

		test_must_fail git push -v origin >output 2>&1 &&
		(
			cd "$REMOTE_REPO" &&
			echo "$HEAD" >expect &&
			git rev-parse --verify HEAD >actual &&
			test_cmp expect actual
		)
	'

	test_expect_success 'non-fast-forward push show ref status' '
		grep "^ ! \[rejected\][ ]*$BRANCH -> $BRANCH (non-fast-forward)$" output
	'

	test_expect_success 'non-fast-forward push shows help message' '
		test_grep "Updates were rejected because" output
	'

	test_expect_${EXPECT_CAS_RESULT} 'force with lease aka cas' '
		HEAD=$(	cd "$REMOTE_REPO" && git rev-parse --verify HEAD ) &&
		test_when_finished '\''
			(cd "$REMOTE_REPO" && git update-ref HEAD "$HEAD")
		'\'' &&
		(
			cd "$LOCAL_REPO" &&
			git push -v --force-with-lease=$BRANCH:$HEAD origin
		) &&
		git rev-parse --verify "$BRANCH" >expect &&
		(
			cd "$REMOTE_REPO" && git rev-parse --verify HEAD
		) >actual &&
		test_cmp expect actual
	'
}

setup_askpass_helper() {
	test_expect_success 'setup askpass helper' '
		write_script "$TRASH_DIRECTORY/askpass" <<-\EOF &&
		echo >>"$TRASH_DIRECTORY/askpass-query" "askpass: $*" &&
		case "$*" in
		*Username*)
			what=user
			;;
		*Password*)
			what=pass
			;;
		esac &&
		cat "$TRASH_DIRECTORY/askpass-$what"
		EOF
		GIT_ASKPASS="$TRASH_DIRECTORY/askpass" &&
		export GIT_ASKPASS &&
		export TRASH_DIRECTORY
	'
}

set_askpass () {
	>"$TRASH_DIRECTORY/askpass-query" &&
	echo "$1" >"$TRASH_DIRECTORY/askpass-user" &&
	echo "$2" >"$TRASH_DIRECTORY/askpass-pass"
}

set_netrc () {
	# $HOME=$TRASH_DIRECTORY
	echo "machine $1 login $2 password $3" >"$TRASH_DIRECTORY/.netrc"
}

clear_netrc () {
	rm -f "$TRASH_DIRECTORY/.netrc"
}

expect_askpass () {
	dest=$HTTPD_DEST${3+/$3}

	{
		case "$1" in
		none)
			;;
		pass)
			echo "askpass: Password for '$HTTPD_PROTO://$2@$dest': "
			;;
		both)
			echo "askpass: Username for '$HTTPD_PROTO://$dest': "
			echo "askpass: Password for '$HTTPD_PROTO://$2@$dest': "
			;;
		*)
			false
			;;
		esac
	} >"$TRASH_DIRECTORY/askpass-expect" &&
	test_cmp "$TRASH_DIRECTORY/askpass-expect" \
		 "$TRASH_DIRECTORY/askpass-query"
}

strip_access_log() {
	sed -e "
		s/^.* \"//
		s/\"//
		s/ [1-9][0-9]*\$//
		s/^GET /GET  /
	" "$HTTPD_ROOT_PATH"/access.log
}

# Requires one argument: the name of a file containing the expected stripped
# access log entries.
check_access_log() {
	sort "$1" >"$1".sorted &&
	strip_access_log >access.log.stripped &&
	sort access.log.stripped >access.log.sorted &&
	if ! test_cmp "$1".sorted access.log.sorted
	then
		test_cmp "$1" access.log.stripped
	fi
}
