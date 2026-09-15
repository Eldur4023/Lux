#!/usr/bin/env bash
#
# Suite for Lux's `http` native module (NATIVE-MODULES.md).
#
# Separate from run_tests.sh for the same reason run_sqlite.sh/run_pdf.sh
# are: libcurl is an OPTIONAL compiled-in dependency (LUX_HTTP), and a
# binary built without it has to skip this suite, not fail it.
#
# Calls a local echo server (http_echo_server.py) instead of the real
# internet: deterministic, no network flakiness -- still a real TCP
# connection and a real HTTP/1.1 round trip through libcurl, not a mock.
#
#   tests/run_http.sh [path-to-binary]

set -u

LUX="${1:-$HOME/lux-build/lux}"
case "$LUX" in /*) ;; *) LUX="$(cd "$(dirname "$LUX")" && pwd)/$(basename "$LUX")" ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUX_TEST_HTTP_PORT:-8840}
ECHO_PORT=${LUX_TEST_HTTP_ECHO_PORT:-8899}
SRV=""
ECHO_SRV=""

passed=0
failed=0

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
grey()  { printf '\033[90m%s\033[0m\n' "$*"; }

ok()   { passed=$((passed + 1)); printf '  ok    %s\n' "$1"; }
fail() {
    failed=$((failed + 1))
    red "  FAIL $1"
    printf '        expected: %s\n        got: %s\n' "$2" "$3"
}

stop_all() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    [ -n "$ECHO_SRV" ] && kill -9 "$ECHO_SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
    wait "$ECHO_SRV" 2>/dev/null
    SRV=""; ECHO_SRV=""
}
trap 'stop_all; rm -rf "$TMP"' EXIT

if ! "$LUX" --check "$HERE/cases/http.lux" > "$TMP/check" 2>&1; then
    # The exact wording project.cpp uses when a module's cmake option was
    # off/its dependency was missing at build time (LUX_HTTP) -- NOT
    # "...contains 'import'", which this message never does (found the hard
    # way: that check, inherited from the older sqlite/postgres/mysql
    # scripts, never actually skips, because it was never exercised against
    # a real missing-module build until this suite's own first run).
    if grep -q "module 'http' is not compiled into this binary" "$TMP/check"; then
        grey "http: the binary was built without the module — suite skipped"
        exit 77
    fi
    red "the test file does not compile:"; cat "$TMP/check"; exit 1
fi

check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}"
    local got
    got=$(curl -sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}' \
          "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body; body=$(head -c 600 "$TMP/body" 2>/dev/null)
    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"; return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"; return
    fi
    ok "$name"
}

echo "== startup =="
python3 "$HERE/http_echo_server.py" "$ECHO_PORT" > "$TMP/echo.log" 2>&1 &
ECHO_SRV=$!
for _ in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$ECHO_PORT/echo" 2>/dev/null && break
    sleep 0.2
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$ECHO_PORT/echo" || {
    red "the echo server did not answer"; cat "$TMP/echo.log"; exit 1; }
ok "echo server starts"

cd "$HERE/.."
"$LUX" --no-watch --port "$PORT" "$HERE/cases/http.lux" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/get_basic" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/get_basic" || {
    red "the server did not answer"; cat "$TMP/srv.log"; exit 1; }
ok "lux server starts and connects"

echo "== requests =="
check "GET with query string"     GET /get_basic          200 '"method":"GET","path":"/echo?x=1"'
check "GET with a custom header"  GET /get_with_headers   200 '"x-test":"hello"'
check "POST with a Dict body (auto JSON + content-type)" GET /post_json 200 '"body":{"name":"ana","age":30}'
check "POST content-type set automatically" GET /post_json 200 '"content-type":"application/json"'
check "POST with a raw string body"         GET /post_raw_string 200 '"body":"plain text body"'
check "PUT method"                GET /put_json           200 '"method":"PUT"'
check "PUT body"                  GET /put_json           200 '"body":{"updated":true}'
check "PATCH method"              GET /patch_json         200 '"method":"PATCH"'
check "PATCH body"                GET /patch_json         200 '"body":{"patched":true}'
check "DELETE"                    GET /delete_basic       200 '"method":"DELETE"'

echo "== status codes pass through =="
check "404 from the remote server" GET /status_404 200 '"status":404'
check "500 from the remote server" GET /status_500 200 '"status":500'

echo "== error paths =="
check "invalid url"          GET /bad_url            500 'url must start with'
check "connection refused"   GET /connection_refused 500 'http.get()'

echo
if [ "$failed" -eq 0 ]; then
    green "$passed tests, all passing"
    exit 0
fi
red "$passed passed, $failed failed"
exit 1
