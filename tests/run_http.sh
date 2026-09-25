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

python3 "$HERE/smtp_sink.py" 8898 "$TMP/mail.json" > "$TMP/smtp.log" 2>&1 &
EXTRA_PIDS="$EXTRA_PIDS $!"

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
# A failed call raises, like any runtime error: uncaught it is a 500 with
# the message; `try` catches it.
check "invalid url"          GET /bad_url            500 '"error":"http.get(): url must start with'
check "connection refused"   GET /connection_refused 500 '"error":"http.get()'
check "public_only refuses a private address" GET /public_only 200 '"blocked":"http.get(): the address is not public (public_only)","allowed_status":200'
check "timeout_ms"           GET /timeout            500 '"error":"http.get(): Timeout'

echo "== bodies and downloads =="
check "form: true sends a urlencoded form" GET /form_post 200 '"body":"grant_type=code\u0026redirect=a%20b%26c","type":"application/x-www-form-urlencoded"'
check "files: sends multipart/form-data"  GET /multipart 200 'multipart/form-data; boundary='
check "multipart carries the field and the file" GET /multipart 200 'name=\"doc\"; filename=\"range_fixture.bin\"'
check "save_to streams the body to a file" GET /save_to 200 '"saved_positive":true,"body":null'
saved=$(python3 -c "import json; print(json.load(open('/tmp/lux-http-save-test.json'))['path'])" 2>&1); rm -f /tmp/lux-http-save-test.json
if [ "$saved" = "/echo?saved=1" ]; then ok "the saved file is the response"; else fail "the saved file is the response" "/echo?saved=1" "$saved"; fi

echo "== mail =="
check "mail.send delivers" GET /mail_send 200 '"r":true'
mail_check=$(python3 -c "
import json, email, sys
m = json.load(open(sys.argv[1]))
assert m['from'] == '<tests@lux.local>', m['from']
assert m['rcpt'] == ['<ana@example.com>', '<bob@example.com>', '<hidden@example.com>'], m['rcpt']
msg = email.message_from_string(m['data'])
assert 'hidden' not in m['data'].split('\r\n\r\n')[0], 'bcc leaked into the headers'
assert msg['Bcc'] is None and 'evil' not in str(msg.keys()), 'header injection'
assert str(email.header.make_header(email.header.decode_header(msg['Subject']))).startswith('Café'), msg['Subject']
parts = {p.get_content_type(): p.get_payload(decode=True).decode() for p in msg.walk() if not p.is_multipart()}
assert parts == {'text/plain': 'plain body', 'text/html': '<b>html body</b>'}, parts
assert msg['Reply-To'] == 'help@example.com'
print('ok')" "$TMP/mail.json" 2>&1 | tail -1)
if [ "$mail_check" = "ok" ]; then ok "the message is well formed, bcc hidden, no header injection"
else fail "the message is well formed, bcc hidden, no header injection" "ok" "$mail_check"; fi
check "mail.send with attachments" GET /mail_attach 200 '"r":true'
att_check=$(python3 -c "
import json, email, sys
m = email.message_from_string(json.load(open(sys.argv[1]))['data'])
assert m.get_content_type() == 'multipart/mixed', m.get_content_type()
body, *files = m.get_payload()
assert body.get_content_type() == 'multipart/alternative'
names = [f.get_filename() for f in files]
assert names == ['http_echo_server.py', 'datos ñ.csv'], names
assert files[1].get_payload(decode=True) == b'a,b\n1,2\n'
assert files[0].get_payload(decode=True) == open('tests/http_echo_server.py', 'rb').read()
assert files[1].get_content_type() == 'text/csv'
print('ok')" "$TMP/mail.json" 2>&1 | tail -1)
if [ "$att_check" = "ok" ]; then ok "attachments: multipart/mixed, names (RFC 2231), bytes intact"
else fail "attachments: multipart/mixed, names (RFC 2231), bytes intact" "ok" "$att_check"; fi
check "mail.send without a recipient" GET /mail_no_rcpt 500 '"error":"mail.send(): no recipient'

echo "== url_encode (RFC 3986, no network) =="
check "space becomes %20, not +"        GET /url_encode 200 '"space":"%20"'
check "UTF-8 bytes escaped per byte"    GET /url_encode 200 '"accented":"%C3%A1"'
check "reserved characters all escaped" GET /url_encode 200 '"reserved":"a%26b%3Fc%23d%2Fe%3Af%25g"'
check "'+' is not special on input"     GET /url_encode 200 '"plus_not_special":"%2B"'
check "unreserved set passes through"   GET /url_encode 200 '"unreserved_untouched":"AZaz09-_.~"'
check "a full query value round-trips"  GET /url_encode 200 '"full_query_value":"hello%20world%20%26%20more%20%3Dtest"'

summary
