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

PORT=${LUX_TEST_HTTP_PORT:-8840}
ECHO_PORT=${LUX_TEST_HTTP_ECHO_PORT:-8899}
source "$(dirname "$0")/lib.sh"

skip_without_module http "$HERE/cases/http.lux"

echo "== startup =="
python3 "$HERE/http_echo_server.py" "$ECHO_PORT" > "$TMP/echo.log" 2>&1 &
EXTRA_PIDS=$!
for _ in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$ECHO_PORT/echo" 2>/dev/null && break
    sleep 0.2
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$ECHO_PORT/echo" || {
    red "the echo server did not answer"; cat "$TMP/echo.log"; exit 1; }
ok "echo server starts"

cd "$HERE/.."
start_server "$HERE/cases/http.lux" /get_basic || exit 1
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

echo "== url_encode (RFC 3986, no network) =="
check "space becomes %20, not +"        GET /url_encode 200 '"space":"%20"'
check "UTF-8 bytes escaped per byte"    GET /url_encode 200 '"accented":"%C3%A1"'
check "reserved characters all escaped" GET /url_encode 200 '"reserved":"a%26b%3Fc%23d%2Fe%3Af%25g"'
check "'+' is not special on input"     GET /url_encode 200 '"plus_not_special":"%2B"'
check "unreserved set passes through"   GET /url_encode 200 '"unreserved_untouched":"AZaz09-_.~"'
check "a full query value round-trips"  GET /url_encode 200 '"full_query_value":"hello%20world%20%26%20more%20%3Dtest"'

summary
