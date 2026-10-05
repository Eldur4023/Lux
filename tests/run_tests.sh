#!/usr/bin/env bash
#
# Regression suite for Lux.
#
# Runs the binary against real .lux files and checks the responses.
# It is written in shell on purpose: it tests the binary the way it is used, over
# the socket, without linking anything from the project.
#
#   tests/run_tests.sh [path-to-binary]
#
# Exits with 0 if everything passes.

PORT=${LUX_TEST_PORT:-8790}
source "$(dirname "$0")/lib.sh"
# For the `os` module's file-I/O tests (cases/modules.lux): a writable
# scratch dir the server can see via os.getenv(), removed with $TMP.
export LUX_TEST_TMP="$TMP"
trap 'kill_wait $SRV; rm -rf "$TMP"; rm -f "$HERE/cases/tests-suite.db"*' EXIT

# Every suite gets a fresh port (see start_server in lib.sh).
next_server() { PORT=$((PORT + 1)); start_server "$1"; }

# check_mp <name> <path> <expected-code> <expected-substring> -- <curl -F flags...>
# Separate from check(): a multipart body is not a string you can pass
# with -d, it is named parts, and curl builds them with one -F per field.
check_mp() {
    local name="$1" path="$2" want_code="$3" needle="${4:-}"; shift 4
    local got
    got=$(curl -sS --max-time 10 -X POST -o "$TMP/body" -w '%{http_code}' "$@" \
          "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body
    body=$(cat "$TMP/body" 2>/dev/null)

    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"
        return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"
        return
    fi
    ok "$name"
}

# fails_to_compile <name> <file> <error-substring>
fails_to_compile() {
    local name="$1" fich="$2" needle="$3"
    local out
    out=$("$LUX" --check "$fich" 2>&1)
    if [ $? -eq 0 ]; then
        fail "$name" "a compile error" "it compiled without complaining"
        return
    fi
    if ! printf '%s' "$out" | grep -qF "$needle"; then
        fail "$name" "an error containing '$needle'" "$(printf '%s' "$out" | head -2)"
        return
    fi
    ok "$name"
}

compiles() {
    local name="$1" fich="$2"
    if "$LUX" --check "$fich" > "$TMP/check" 2>&1; then
        ok "$name"
    else
        fail "$name" "it to compile" "$(head -3 "$TMP/check")"
    fi
}

# native_compiles <name> <file> <routes>: --native really compiles at least
# <routes> routes of it. A g++ failure falls back to bytecode silently and
# every test still passes (same results), so only this notices.
native_compiles() {
    local name="$1" fich="$2" want="$3" got
    got=$(cd "$TMP" && "$LUX" --native --check "$fich" 2>&1)
    local n
    n=$(printf '%s' "$got" | sed -n 's/.* \([0-9]*\) route(s) compiled to native code.*/\1/p')
    if [ -n "$n" ] && [ "$n" -ge "$want" ] && ! printf '%s' "$got" | grep -q "could not be compiled"; then ok "$name"
    else fail "$name" ">= $want native routes, no g++ failure" "$(printf '%s' "$got" | grep -E 'compiled to native|could not|error' | head -3)"; fi
}

# ─── Suites ──────────────────────────────────────────────────────────────────

echo "== language =="
next_server "$HERE/cases/language.lux" || exit 1
check "arithmetic"          GET /arithmetic       200 '"sum":7'
check "strings"             GET /strings          200 '"upper":"HELLO"'
check "multiline without margin" GET /multiline     200 '"sql":"SELECT id\nFROM posts"'
check "one-line multiline"   GET /multiline       200 '"loose":"on one line"'
check "multiline escapes"  GET /multiline       200 '"escapes":"with \"quotes\""'
check "truthiness"           GET /truthiness        200 '"zero":false'
check "List first/last/sum/min/max" GET /list_more 200 '"first":3,"last":6,"none":null,"sum":31,"fsum":3.5,"min":1,"max":9'
check "List unique/any/all/count"   GET /list_more 200 '"unique":[3,1,4,5,9,2,6],"any":true,"all":false,"count":3'
check "List group_by/chunk"         GET /list_more 200 '"groups":{"odd":[3,1,1,5,9],"even":[4,2,6]},"chunks":[[1,2],[3,4],[5]]'
check "float() conversion"          GET /float_conv 200 '"a":3.5,"b":2.0,"c":1000.0,"d":1.0'
check "float() rejects non-numbers"  GET /float_bad  500 "float(): 'abc' is not a number"
check "List pop/insert, Dict items" GET /list_more 200 '"popped":3,"stack":[0,1,9,2],"items":[["a",1],["b",2]]'

# JSON has to be UTF-8 (RFC 8259).  A stray 0xFF arrives from the network by
# several routes, and without sanitizing it produced a response that not even
# the client that sent it could parse: the request does not fail, the receiver does.
json_utf8() {
    local name="$1"; shift
    curl -sS --max-time 10 -o "$TMP/body" "$@" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))"             "$TMP/body" 2>/dev/null; then
        ok "$name"
    else
        fail "$name" "valid UTF-8 JSON" "$(head -c 120 "$TMP/body" | cat -v)"
    fi
}
json_utf8 "broken utf8 in the query"    "http://127.0.0.1:$PORT/echo_query?q=a%FFb"
json_utf8 "broken utf8 in the header" -H "$(printf 'X-Test: aÿb')"           "http://127.0.0.1:$PORT/echo_header"
# And what IS valid has to come out UNTOUCHED: sanitizing cannot spoil good
# text, which is the half that really matters.
check "valid utf8 untouched" GET /echo_valid 200 '"echo":"añoñó 🐻 ñ"'
check "no coercion"        GET /coercion         500 'cannot add'
check "elif conditional"    GET /classify/0      200 '"r":"zero"'
check "else conditional"    GET /classify/99     200 '"r":"big"'
check "while loop"         GET /sum_up_to/10    200 '"total":55'
check "for loop and break"   GET /evens            200 '"evens":[2,4,6]'
check "operators"          GET /operators       200 '"a":2'
check "pre-increment"   GET /increment       200 '"pre":7'
check "indices"             GET /indices          200 '"l":[99,20,25]'
check "ternary"            GET /ternary/20      200 '"role":"adult"'
check "try catches"         GET /captura          200 'division by zero'
check "uncaught error"  GET /boom         500 'division by zero'

check "string.index_of"        GET /strings2 200 '"index_of":7,"index_of_missing":-1'
check "string.replace"         GET /strings2 200 '"replace":"Hell0, W0rld"'
check "string.split"           GET /strings2 200 '"split":["Hello","World"]'
check "string.slice"           GET /strings2 200 '"slice":"Hello","slice_neg":"World"'
check "string.repeat"          GET /strings2 200 '"repeat":"ababab"'
check "List.contains/index_of" GET /lists2   200 '"contains":true,"index_of":2'
check "List.sort"              GET /lists2   200 '"sorted":[1,1,3,4,5]'
check "List.reverse"           GET /lists2   200 '"reversed":[5,1,4,1,3]'
check "List.slice"             GET /lists2   200 '"slice":[20,30]'
check "List.concat"            GET /lists2   200 '"concat":[1,2,3,4]'
check "List.join"              GET /lists2   200 '"join":"1-2-3"'
check "List.remove_at"         GET /list_remove_at 200 '"removed":true,"out_of_range":false,"after":[10,30]'
check "List.sort rejects mixed types" GET /sort_mixed_types 500 'cannot be compared'
check "Dict.values"            GET /dicts2   200 '"values":[1,2]'
check "Dict.get present/missing" GET /dicts2 200 '"get_present":1,"get_missing":-1'
check "Dict.remove"            GET /dicts2   200 '"removed":true,"after_remove":{"b":2}'
check "Dict.merge"             GET /dicts2   200 '"merged":{"x":1,"y":2}'

check "bare fn name is a Func value, not null" GET /func_ref 200 '"is_not_null":true'
check "List.map"               GET /list_map    200 '"doubled":[2,4,6,8]'
check "List.filter"            GET /list_filter 200 '"evens":[2,4,6]'
check "List.reduce"            GET /list_reduce 200 '"sum":15'
check "List.for_each"          GET /list_for_each 200 '"total":6'
check "map/filter chain"       GET /list_map_filter_chain 200 '"r":[4,8,12,16]'
check "callback nesting does not corrupt outer call" GET /list_map_nested 200 '"r":[3,7]'
check "await inside a map() callback is rejected" GET /list_map_await_rejected 500 'used `await`, which is not supported'
check "a plain fn that awaits keeps its return value for the caller" GET /fn_return_after_await 200 '"v":5'
check "same, with a bare 'return await fn()'"                        GET /fn_return_after_await_bare 200 '5'
check "has_await propagates two calls deep"                          GET /fn_transitively_awaits 200 '"v":15'
check "chaining a method call straight off a function/constructor result" GET /chain_call_method 200 '"r":25'

check "range(n)"              GET /range 200 '"one_arg":[0,1,2,3,4]'
check "range(start, end)"     GET /range 200 '"two_args":[2,3,4,5]'
check "range with step"       GET /range 200 '"step":[0,2,4,6,8]'
check "range with negative step" GET /range 200 '"negative_step":[5,4,3,2,1]'
check "range() in a for loop" GET /range_for_loop 200 '"total":10'
check "range() rejects step 0" GET /range_bad_step 500 'step cannot be 0'
check "for over range(): shapes" GET /range_for_shapes 200 '"down":[5,3,1],"empty":0,"n":33'
check "len() counts codepoints" GET /len_utf8 200 '"len":29,"long":4000'
check "for over range(): step 0" GET /range_for_bad_step 500 'step cannot be 0'

check "switch basic case"      GET /switch_basic/1 200 '"r":"one"'
check "switch else fallback"   GET /switch_basic/99 200 '"r":"many"'
check "switch no else, no match leaves variable untouched" GET /switch_no_else/99 200 '"r":"unset"'
check "switch multi-value case" GET /switch_multi_value/2 200 '"r":"low"'
check "switch multi-value case, second group" GET /switch_multi_value/5 200 '"r":"mid"'
check "switch subject evaluated exactly once" GET /switch_eval_once 200 '"r":"zero","calls":1'
check "enum member is a plain string" GET /enum_basic 200 '"c":"RED","s":"ACTIVE"'
check "comparing a string against an enum member" GET /enum_compare/RED 200 '"is_red":true'
check "switch over an enum-valued variable" GET /enum_switch 200 '"label":"in progress"'
check "class validate: rule using an enum, valid" POST /enum_order 200 '"status":"ACTIVE"' '{"status":"ACTIVE"}'
check "class validate: rule using an enum, invalid" POST /enum_order 422 'status: invalid' '{"status":"BOGUS"}'

echo "== routes and parameters =="
next_server "$HERE/cases/routes.lux" || exit 1
check "path parameter"   GET /echo/42           200 '"id":42'
check "query with default"   GET /pagina           200 '"page":1'
check "query explicita"     GET '/pagina?page=7'  200 '"page":7'
check "invalid type"       GET /echo/abc          400 'invalid parameter'
check "a response over 16 MB" GET /big             200 'xxxxxxxx'
check "path inexistente"    GET /nada             404 'not found'
check "group with prefix"   GET /api/v1/hello      200 '"v":1'
check "guard denies"      GET /admin/panel      403
check "guard allows"      GET '/admin/panel?k=abre' 200 '"panel":true'
check "404 handler"       GET /tampoco          404 '"path":"/tampoco"'
check "a route's own 404 body is kept" GET /own_404 404 '"id":7'
check "a bare status(404) gets the handler" GET /bare_404 404 '"path":"/bare_404"'
check "method words are names outside a route" GET /method_names 200 '"post":"p","delete":2'

# ── Parameter binding matrix ──────────────────────────────────────────────
# Origen (path / query / multipart) x type (escalar, File, List<File>) x
# presence (missing, mistyped, in two places at once).  See cases/params.lux:
# the real bug was a text parameter ALWAYS empty on a route with files,
# and it only shows up when the two live on the same route -- testing query and
# multipart separately, as the rest of the suite did, never caught it.
echo "== parameter binding =="
next_server "$HERE/cases/params.lux" || exit 1

check "missing query param with no default is a 422, not the type's zero" \
    GET /query 422 '"q: required"'
check "query param present, the other one falls back to its default" \
    GET "/query?q=hi" 200 '"q":"hi","n":5'

check_mp "multipart: text next to a file" /mp/one 200 '"title":"hello"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "title=hello"
check_mp "multipart: the file arrives too" /mp/one 200 '"filename":"a.txt"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "title=hello"
check_mp "File.sha256() is the upload's hash" /mp/sha 200 "\"sha256\":\"$(sha256sum "$HERE/cases/params.lux" | cut -d' ' -f1)\"" \
    -F "f=@$HERE/cases/params.lux;filename=a.txt"

check_mp "multipart: string"  /mp/types 200 '"s":"hello"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"
check_mp "multipart: int"     /mp/types 200 '"n":7' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"
check_mp "multipart: bool"    /mp/types 200 '"b":true' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "s=hello" -F "n=7" -F "b=true"

check_mp "multipart: text next to a List<File>" /mp/list 200 '"album":"holidays"' \
    -F "fs=@$HERE/cases/params.lux;filename=a.txt" -F "album=holidays"
check_mp "multipart: counts the files in the list" /mp/list 200 '"n":2' \
    -F "fs=@$HERE/cases/params.lux;filename=a.txt" \
    -F "fs=@$HERE/cases/params.lux;filename=b.txt" -F "album=x"

check_mp "multipart: missing field falls back to the default" /mp/default 200 '"label":"no-label"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt"

check_mp "multipart: the query beats the form field" "/mp/prioridad?origin=query" 200 '"origin":"query"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "origin=formulario"

check_mp "multipart: mistyped scalar gives 400" /mp/bad 400 'invalid parameter' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "n=no-es-un-number"

echo "== classes and validation =="
next_server "$HERE/cases/classes.lux" || exit 1
check "valid body"       POST /add 201 '"creado":"Ana"' '{"name":"Ana","age":30}'
check "field required"   POST /add 422 'age: required' '{"name":"Ana"}'
check "wrong type"     POST /add 422 'expected int' '{"name":"Ana","age":"30"}'
check "broken rule"    POST /add 422 'must be of age' '{"name":"Ana","age":10}'
check "every message"  POST /add 422 'name: required' '{"name":"","age":10}'
check "invalid json"       POST /add 400 'invalid JSON' '{roto'
check "messages in on error" POST /add 422 '"count":2' '{"name":"","age":10}'
check "constructor"         GET /punto/3/4        200 '"cuadrado":25'
check "method with a default"  GET /label/1/2     200 '"other":"Q(1,2)"'
check "user function"  GET /doble/21         200 '"r":42'
check "recursion"           GET /factorial/5      200 '"r":120'
check "recursion cap"   GET /infinita         500 'too much recursion'
check "a standalone fn can construct a class and call a method on it" GET /puntos_desde_fn 200 '"cuadrados":[25,2]'

check "class field: List<Class>, built and returned as JSON" GET /nested/build 200 '"episodes":[{"title":"Pilot","duration_s":1320},{"title":"Episode 2","duration_s":1290}]'
check "class field: List<Class>, forward reference (Season before Episode) resolves" GET /nested/build 200 '"name":"Season 1"'
check "class field: indexing/iterating/len() over a List<Class> field" GET /nested/access 200 '"first_title":"Pilot","total":2610,"count":2'
check "class field: Episode? present, serialized inline"  GET /nested/optional_present 200 '"featured":{"title":"Pilot","duration_s":1320}'
check "class field: Episode? absent is null, not omitted" GET /nested/optional_absent  200 '"featured":null'
check "class field: POST body binds a nested List<Class> recursively" POST /nested/ingest 200 '"name":"S1","count":1,"total":100' '{"name":"S1","episodes":[{"title":"A","duration_s":100}]}'
check "class field: POST body rejects a wrong type INSIDE a nested element" POST /nested/ingest 422 'episodes: expected List' '{"name":"S1","episodes":[{"title":"A","duration_s":"oops"}]}'
check "class field: POST body rejects a missing field INSIDE a nested element" POST /nested/ingest 422 'episodes: expected List' '{"name":"S1","episodes":[{"title":"A"}]}'
check "class field: POST body rejects the list itself being the wrong shape" POST /nested/ingest 422 'episodes: expected List' '{"name":"S1","episodes":"not-a-list"}'

echo "== session and jwt =="
next_server "$HERE/cases/session.lux" || exit 1
check "no session"          GET /whoami           200 '"user":null'
check "protected area"      GET /admin/panel      403
check "forged cookie"  GET /whoami           200 '"user":null'
check "jwt missing"         GET /api/me           401
check "invalid jwt"        GET /api/me           401
# A session written, read back and cleared through the cookie, and a valid
# token: the round trips, not only the refusals.
jar="$TMP/jar"
curl -sS -c "$jar" -X POST "http://127.0.0.1:$PORT/login?user=ana&role=admin" > /dev/null
got=$(curl -sS -b "$jar" "http://127.0.0.1:$PORT/whoami")
case "$got" in *'"user":"ana","role":"admin"'*) ok "session written and read back" ;; *) fail "session written and read back" 'user ana' "$got" ;; esac
code=$(curl -sS -b "$jar" -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/admin/panel")
[ "$code" = 200 ] && ok "a session opens the protected area" || fail "a session opens the protected area" 200 "$code"
curl -sS -b "$jar" -c "$jar" -X POST "http://127.0.0.1:$PORT/logout" > /dev/null
got=$(curl -sS -b "$jar" "http://127.0.0.1:$PORT/whoami")
case "$got" in *'"user":null'*) ok "session.clear() logs out" ;; *) fail "session.clear() logs out" 'user null' "$got" ;; esac
token=$(python3 -c "
import hmac, hashlib, base64, json, time
b = lambda d: base64.urlsafe_b64encode(d).rstrip(b'=').decode()
h = b(json.dumps({'alg': 'HS256', 'typ': 'JWT'}).encode())
p = b(json.dumps({'sub': 'u42', 'iss': 'tests', 'exp': int(time.time()) + 600}).encode())
sig = hmac.new(b'test-jwt-secret-long-enough-for-hmac-signing', (h + '.' + p).encode(), hashlib.sha256).digest()
print(h + '.' + p + '.' + b(sig))")
got=$(curl -sS -H "Authorization: Bearer $token" "http://127.0.0.1:$PORT/api/me")
case "$got" in *'"sub":"u42"'*) ok "a valid jwt" ;; *) fail "a valid jwt" 'sub u42' "$got" ;; esac
# jwt.sign issues what jwt.valid accepts; jwt.verify checks any token.
signed=$(curl -sS -X POST "http://127.0.0.1:$PORT/token" | python3 -c "import json,sys; print(json.load(sys.stdin)['token'])")
got=$(curl -sS -H "Authorization: Bearer $signed" "http://127.0.0.1:$PORT/api/me")
case "$got" in *'"sub":"u7"'*) ok "jwt.sign issues what jwt.valid accepts" ;; *) fail "jwt.sign issues what jwt.valid accepts" 'sub u7' "$got" ;; esac
check "jwt.verify: a signed token, with its issuer" GET "/verify?token=$signed" 200 '"valid":true,"sub":"u7","iss":"tests"'
check "jwt.verify: a tampered one"  GET "/verify?token=${signed%?}x" 200 '"valid":false'
check "jwt.verify: garbage"         GET "/verify?token=abc" 200 '"valid":false'
check "an sse guard answers 401 before the stream" GET "/live/feed?token=bad" 401
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 3 -H "Connection: Upgrade" -H "Upgrade: websocket" \
       -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Origin: http://t.test" \
       "http://127.0.0.1:$PORT/live/chat?token=bad")
[ "$code" = 401 ] && ok "a ws guard answers 401 before the upgrade" || fail "a ws guard answers 401 before the upgrade" 401 "$code"
code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 1 -H "Connection: Upgrade" -H "Upgrade: websocket" \
       -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Origin: http://t.test" \
       "http://127.0.0.1:$PORT/live/chat?token=$signed")
[ "$code" = 101 ] && ok "and upgrades with a good token" || fail "and upgrades with a good token" 101 "$code"

echo "== database =="
rm -f "$HERE/cases/tests-suite.db"*
next_server "$HERE/cases/data.lux" || exit 1
check "create table"         GET  /create           200 '"ok":true'
check "insert"            POST /add/ana        201 '"id":1'
check "insert another"       POST /add/bob        201 '"id":2'
check "list"              GET  /all           200 '"name":"ana"'
check "lookup by id"       GET  /one/1           200 '"name":"ana"'
check "not found"       GET  /one/99          404
check "sql injection"       GET  "/search?q=ana'%20OR%20'1'='1" 200 '"found":0'
check "transaction"         POST /transfer      200 '"ok":true'
check "balances after commit"  GET  /balances          200 '"balance":70'
check "rollback"            POST /undo         200 '"undone":true'
check "balances after rollback" GET /balances          200 '"balance":70'
check "engine error"     GET  /bad            500 'no such table'

echo "== native modules =="
next_server "$HERE/cases/modules.lux" || exit 1
check "hash.sha256"       GET /hash/test              200 '"sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"'
check "hash.hmac_sha256"  GET /hmac/mykey/mymsg       200 '"mac":"131b7d67f083953569aa6d777328a10cf618900e9d261397d750ec30df3c9654"'
check "hash.random_hex length" GET /random/8         200
random_hex_len=$(curl -sS --max-time 10 "http://127.0.0.1:$PORT/random/8" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['hex']))")
if [ "$random_hex_len" = "16" ]; then ok "hash.random_hex(8) is 16 hex chars"
else fail "hash.random_hex(8) is 16 hex chars" "16" "$random_hex_len"; fi
check "hash.password/verify round trip"     GET /hash/password 200 '"format":true,"good":true,"bad":false'
check "hash.verify reads Django's format"   GET /hash/password 200 '"django":true,"garbage":false'
check "hash.sign/unsign round trip"         GET /hash/sign     200 '"back":{"user":7},"tampered":null,"wrong_key":null'
check "hash.unsign honours max_age"         GET /hash/sign     200 '"fresh":{"user":7},"expired":null'
check "hash.uuid v4 and v7"                 GET /hash/ids      200 '"len":36,"v4":"4","v7":"7"'
check "hash.token/equal"                    GET /hash/ids      200 '"token_len":43,"same":true,"differ":false'
check "a module signature rejects a wrong type" GET /hash/bad_arg 500 'hash.sha256(): argument 1 must be a string, not int'
check "json.stringify with indent" GET /json/pretty 200 '    "d": null'
check "json.stringify keeps empty containers inline" GET /json/pretty 200 '  "b": {},'
check "encoding base64/hex/url round trips" GET /encoding/basic 200 '"b64":"aGk/Pg==","b64url":"aGk_Pg","back":"hi?\u003e","hex":"ok","url":"a b/ñ"'
check "encoding.query_encode/decode"         GET /encoding/basic 200 '"query":"q=a%20b\u0026tag=x\u0026tag=y","parsed":{"a":"1","b":"x y"}'
check "encoding.html_escape"                 GET /encoding/basic 200 '"html":"\u0026lt;a href=\u0026#39;x\u0026#39;\u0026gt;"'
check "encoding.url_parse, absolute"         GET /encoding/url 200 '"full":{"scheme":"https","host":"a.com","port":8080,"path":"/p/x","query":{"q":"1"},"fragment":"top"}'
check "encoding.url_parse, relative"         GET /encoding/url 200 '"relative":{"scheme":"","host":"","port":null,"path":"/next"'
check "encoding.url_parse reads /\\host as a host" GET /encoding/url 200 '"sneaky":{"scheme":"","host":"evil.com"'
check "encoding.hex_decode rejects bad hex"  GET /encoding/bad_hex 500 'invalid hex'
check "text.slug transliterates"        GET /text/basic 200 '"slug":"cafe-con-leche-2024-lodz","slug2":"strasse-aesir-oeuvre"'
check "text.truncate counts the suffix" GET /text/basic 200 '"trunc":"Hello w…","short":"Hi"'
check "text.format_number"              GET /text/basic 200 '"num":"1,234,567.89","num_es":"-1.234.567,89"'
check "text.format_number rounds in decimal" GET /text/basic 200 '"half_up":"2.68","carry":"1,000.00","exact":"1,234.57","int":"1,234,567","neg_zero":"0.00"'
check "text.pad_left/pad_right"         GET /text/basic 200 '"pad":"0007","padr":"ñ.."'
check "text.distance"                   GET /text/basic 200 '"dist":3'
check "state.incr with a TTL counts in a fixed window" GET /state/ttl 200 '"a":1,"b":2,"after_window":1'
check "state.set with a TTL expires"   GET /state/ttl   200 '"before":"abc","expired":null,"default":"none"'
check "state set/get/incr/decr/remove" GET /state/plain 200 '"k":{"x":1},"n":-2,"removed":true,"gone":null'
check "net.ip_in v4/v6/lists"    GET /net/basic 200 '"in":true,"out":false,"list":true,"v6net":true,"v4_in_v6":false,"bad":false'
check "net.is_private"           GET /net/basic 200 '"priv":true,"meta":true,"pub":false,"ula":true'
check "net.ip_version"           GET /net/basic 200 '"v4":4,"v6":6,"none":null'
# Keep-alive: a connection idle past the header timeout (5s) is still open
# -- it used to get a 408 then, taken by the client's next request as its
# answer -- while a request begun and left unfinished still gets the 408.
timeouts=$(python3 -c "
import socket, sys, threading, time
port = int(sys.argv[1]); out = {}
def idle():
    s = socket.create_connection(('127.0.0.1', port)); s.settimeout(10)
    s.sendall(b'GET /net/basic HTTP/1.1\\r\\nHost: t\\r\\n\\r\\n'); s.recv(65536)
    time.sleep(6)
    s.sendall(b'GET /net/basic HTTP/1.1\\r\\nHost: t\\r\\n\\r\\n')
    out['idle'] = s.recv(65536)[:12]
def partial():
    s = socket.create_connection(('127.0.0.1', port)); s.settimeout(10)
    s.sendall(b'GET /net/basic HTTP/1.1\\r\\nHo')
    out['partial'] = s.recv(65536)[:12]
ts = [threading.Thread(target=f) for f in (idle, partial)]
[t.start() for t in ts]; [t.join() for t in ts]
print(out.get('idle'), out.get('partial'))" "$PORT" 2>&1 | tail -1)
if [ "$timeouts" = "b'HTTP/1.1 200' b'HTTP/1.1 408'" ]; then ok "keep-alive idle past 5s stays open; a partial request gets 408"
else fail "keep-alive idle past 5s stays open; a partial request gets 408" "b'HTTP/1.1 200' b'HTTP/1.1 408'" "$timeouts"; fi
check "zip.create writes the archive" GET /zip/create 200 '"n":2'
zip_path=$(curl -sS "http://127.0.0.1:$PORT/zip/create" | python3 -c "import json,sys; print(json.load(sys.stdin)['path'])")
zip_check=$(python3 -c "
import zipfile, sys
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
assert z.namelist() == ['a.txt', 'evil/b.bin'], z.namelist()
assert z.read('a.txt') == b'hello zip hello zip hello zip hello zip hello zip'
assert z.getinfo('a.txt').compress_type in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED)
print('ok')" "$zip_path" 2>&1 | tail -1)
rm -rf "$(dirname "$zip_path")"
if [ "$zip_check" = "ok" ]; then ok "zip archive is valid, names are sanitised"
else fail "zip archive is valid, names are sanitised" "ok" "$zip_check"; fi
zip_path=$(curl -sS --max-time 30 "http://127.0.0.1:$PORT/zip/many" | python3 -c "import json,sys; print(json.load(sys.stdin)['path'])")
zip_check=$(python3 -c "
import zipfile, sys
z = zipfile.ZipFile(sys.argv[1])
names = z.namelist()
assert len(names) == 70000, len(names)
assert z.read('f69999.txt') == b'x'
print('ok')" "$zip_path" 2>&1 | tail -1)
rm -rf "$(dirname "$zip_path")"
if [ "$zip_check" = "ok" ]; then ok "zip writes ZIP64 past 65535 entries"
else fail "zip writes ZIP64 past 65535 entries" "ok" "$zip_check"; fi
sleep 2.3
ticks=$(curl -sS "http://127.0.0.1:$PORT/every/ticks" | python3 -c "import json,sys; print(json.load(sys.stdin)['ticks'])")
if [ "$ticks" -ge 2 ] 2>/dev/null; then ok "every \"1s\" runs on its own ($ticks runs)"
else fail "every \"1s\" runs on its own" ">= 2 runs" "$ticks"; fi
fast=$(curl -sS "http://127.0.0.1:$PORT/every/ticks" | python3 -c "import json,sys; print(json.load(sys.stdin)['fast'])")
if [ "$fast" -ge $((ticks * 3)) ] 2>/dev/null; then ok "every \"200ms\" runs at its own pace ($fast runs)"
else fail "every \"200ms\" runs at its own pace" ">= $((ticks * 3)) runs" "$fast"; fi
kill -0 "$SRV" 2>/dev/null && ok "a failing task does not take the server down" \
                           || fail "a failing task does not take the server down" "alive" "dead"
check "every is not reachable over HTTP" GET /__every/0 404 'Not Found'
check "handles cannot be guessed by counting" GET /handles 200 '"not_small":true,"not_sequential":true'
check "csv.parse/columns/rows"     GET /csv/basic                 200 '"name":"ana","age":30,"city":"madrid"'
check "csv row_count"              GET /csv/basic                 200 '"row_count":3'
check "csv.read + List.filter"     GET /csv/read_and_aggregate 200 '"madrid":[{"name":"ana","age":30,"city":"madrid"},{"name":"cleo","age":35,"city":"madrid"}]'
check "csv.read + List.reduce"     GET /csv/read_and_aggregate 200 '"total_age":90'
check "csv.read drops the BOM, takes ';'" GET /csv/excel 200 '"rows":[{"nombre":"Pérez; J.","precio":"1,5"}]'
check "csv.write quotes the delimiter"    GET /csv/excel 200 '"out":"nombre;precio\r\n\"Pérez; J.\";1,5\r\n"'
check "csv.write from Dicts, formulas defused" GET /csv/write 200 "\"dicts\":\"a,b\\r\\n1,\\\"x,y\\\"\\r\\n'=cmd,\\r\\n\""
check "csv keeps what a number would lose" GET /csv/types 200 '"row":{"zip":"01234","phone":"+34600111222","n":42,"f":2.5,"big":"1234567890123456789012","neg":-7,"exp":1000.0,"sp":" 5","pi":"3.14159265358979323"}'
check "csv.read typed=false"             GET /csv/types 200 '"raw":"42"'
check "csv.write from Lists with columns"      GET /csv/write 200 '"lists":"p,q\r\n1,2\r\n3,4\r\n"'
check "csv quoted fields (RFC4180)" GET /csv/quoted               200 '"name":"Smith, John","note":"He said \"hi\""'
check "csv.to_csv roundtrip"       GET /csv/roundtrip             200 'a,b\r\n1,2\r\n3,4\r\n'
check "csv closed handle errors"   GET /csv/closed_handle         500 'unknown handle'

check "os.getenv default"          GET /os/env                    200 '"missing_uses_default":"fallback"'
check "os.getenv present"          GET /os/env                    200 '"present_is_not_null":true'
check "os.path_join/basename/dirname" GET /os/path                200 '"joined":"a/b/c.txt","base":"z.txt","dir":"/x/y"'
check "os file round-trip"         GET /os/file_roundtrip         200 '"content":"hola mundo"'
check "os file round-trip cleans up" GET /os/file_roundtrip       200 '"removed":true,"exists_after":false'
check "os.read_file missing is null, not an error" GET /os/missing_file 200 '"content_is_null":true'
check "os.run captures stdout"     GET /os/run_echo               200 '"stdout":"hello from os.run\n"'
check "os.run exit status"         GET /os/run_echo               200 '"status":0'
check "os.run missing command errors" GET /os/run_missing         500 '"error":"os.run(): could not start'

check "os.is_dir/is_file tell a directory from a file" GET /os/stat 200 '"dir_is_dir":true,"dir_is_file":false,"file_is_dir":false,"file_is_file":true'
check "os.file_size reads a real file's size"          GET /os/stat 200 '"size":5'
check "os.file_size on a directory is -1, not a real size" GET /os/stat 200 '"dir_size":-1'
check "os.mtime_ms on a real file is a positive timestamp" GET /os/stat 200 '"mtime_positive":true'
check "os.is_dir/is_file on a missing path are both false" GET /os/stat_missing 200 '"is_dir":false,"is_file":false'
check "os.file_size/mtime_ms on a missing path are -1"     GET /os/stat_missing 200 '"size":-1,"mtime":-1'

check "os.remove_dir removes an empty directory"        GET /os/remove_dir_empty  200 '"removed":true,"exists_after":false'
check "os.remove_dir on a non-empty dir is rejected"     GET /os/remove_dir_nonempty_rejected 500 "is not empty -- pass true"
check "os.remove_dir(true) removes a non-empty tree"     GET /os/remove_dir_recursive 200 '"removed":true,"exists_after":false'
check "os.remove_dir on a file is rejected"              GET /os/remove_dir_on_a_file_rejected 500 "is not a directory"
check "os.copy_file copies without touching the source"  GET /os/copy_file 200 '"copied":true,"src_content":"copy me","dst_content":"copy me"'
check "os.copy_file onto an existing file is rejected"   GET /os/copy_file_overwrite_rejected 500 '"error":"os.copy_file()'
check "os.copy_file(overwrite=true) replaces the destination" GET /os/copy_file_overwrite_allowed 200 '"copied":true,"dst_content":"new"'
check "os.move renames a file"                           GET /os/move_file 200 '"moved":true,"src_gone":true,"dst_content":"moving"'
check "os.move moves a whole directory tree"             GET /os/move_dir 200 '"moved":true,"src_gone":true,"content":"nested"'
check "os.move on a missing source is rejected"          GET /os/move_missing_source_rejected 500 'does not exist'

check "os.run input/cwd/env"         GET /os/run_options 200 '"stdin":"fed through stdin","cwd":"/tmp\n","env":"set\n"'
check "a failed await is catchable (os.run timeout_ms)" GET /os/run_options 200 '"timeout":"os.run(): '"'"'sleep'"'"' timed out after 100ms"'
check "os.append_file/glob/list_dir(recursive)" GET /os/files_more 200 '"appended":"one+two","glob":['
check "os.list_dir recursive is relative" GET /os/files_more 200 '"tree":["a.txt","b.JPG","sub","sub/c.txt"]'
check "os.mime/path_ext/temp_file"    GET /os/files_more 200 '"mime":"image/jpeg","mime_unknown":"application/octet-stream","ext":".gz","tmp_ok":true'
check "proc stdin pipe + write"       GET /proc/stdin 200 '"written":4,"echo":"ping","code":0'
check "math.abs/min/max"           GET /math/basic  200 '"abs":7,"abs_f":2.5,"min":3,"max":9'
check "math.round/floor/ceil"      GET /math/basic  200 '"round":3,"floor":2,"ceil":3'
check "math.sqrt/pow/log"          GET /math/basic  200 '"sqrt":4.0,"pow":1024.0,"log":0.0'
check "math.round(x, digits)/clamp/sign"   GET /math/more   200 '"round2":3.14,"clamp":10,"sign":-1'
check "math.min/max of a List and of many" GET /math/more   200 '"min_list":2,"max3":7,"log2":3.0'
check "math.choice/shuffle/sample"         GET /math/more   200 '"choice_in":true,"shuffled_len":3,"sample_len":2,"untouched":[4,9,2]'
check "math domain error"                  GET /math/domain 500 'math domain error'
check "math.random is in [0,1)"    GET /math/random 200 '"in_range":true'
check "math.random_int inclusive edge" GET /math/random 200 '"fixed_range":5'
check "time.now/now_seconds"       GET /time/basic  200 '"n_positive":true,"s_positive":true'
check "time.format_iso/format epoch0" GET /time/basic 200 '"iso_epoch0":"1970-01-01T00:00:00Z","custom_epoch0":"1970-01-01"'
check "time.parse_iso roundtrip"   GET /time/parse  200 '"parsed_epoch0":0'
check "time.parse_iso invalid is null" GET /time/parse 200 '"bad_is_null":true'

check "time.parts"                 GET /time/calendar 200 '"parts":{"year":2024,"month":1,"day":31,"hour":15,"minute":30,"second":0,"weekday":3,"yearday":31}'
check "time.add_months clamps the day" GET /time/calendar 200 '"feb":"2024-02-29T15:30:00Z","feb_2023":"2023-02-28T00:00:00Z","back":"2022-12-31T15:30:00Z"'
check "time.start_of"              GET /time/calendar 200 '"day":"2024-01-31T00:00:00Z","week":"2024-01-29T00:00:00Z","month":"2024-01-01T00:00:00Z","year":"2024-01-01T00:00:00Z"'
check "time.start_of in an offset" GET /time/calendar 200 '"day_cet":"2024-01-31T23:00:00Z"'
check "time.ago in English"        GET /time/ago 200 '"just":"just now","min":"3 minutes ago","hour":"1 hour ago","future":"in 2 hours"'
check "time.ago in Spanish"        GET /time/ago 200 '"es":"hace 3 días","es_month":"hace 2 meses","es_future":"dentro de 1 minuto"'
check "time.parse_iso ms + offset" GET /time/ago 200 '"ms":1717236000500'
check "regex.test"             GET /regex/test  200 '"yes":true,"no":false'
check "regex.find"             GET /regex/find  200 '"found":"123","missing":null'
check "regex.find_all"         GET /regex/find_all 200 '"all":["1","22","333"]'
check "regex.groups"           GET /regex/groups 200 '"g":["bob@example.com","bob","example"]'
check "regex.replace"          GET /regex/replace 200 '"r":"a# b# c#"'
check "regex.replace backreferences" GET /regex/replace_backref 200 '"r":"host@user"'
check "regex.split"            GET /regex/split 200 '"r":["a","b","c","d"]'
check "regex.escape matches only itself" GET /regex/escape 200 '"self":true,"other":false,"cached":true'
check "regex cache evicts and stays right" GET /regex/many 200 '"hits":300,"first_again":true'
check "regex rejects an invalid pattern" GET /regex/bad_pattern 500 'invalid regex pattern'

check "rooms.count on an unknown room is 0, not an error" GET /rooms/count_empty     200 '"n":0'
check "rooms.broadcast to an unknown room reaches nobody" GET /rooms/broadcast_empty 200 '"reached":0'
check "rooms.join outside a ws route is rejected"         GET /rooms/join_outside_ws 500 'can only be called from a ws or sse route'

rooms_out=$(python3 "$HERE/ws_rooms_check.py" "$PORT" 2>&1 | tail -1)
if [ "$rooms_out" = "ok" ]; then ok "rooms: join, broadcast_others (JSON), count, leave on close"
else fail "rooms: join, broadcast_others (JSON), count, leave on close" "ok" "$rooms_out"; fi
sse_rooms_out=$(python3 "$HERE/sse_rooms_check.py" "$PORT" 2>&1 | tail -1)
if [ "$sse_rooms_out" = "ok" ]; then ok "rooms: sse streams in a room get broadcasts"
else fail "rooms: sse streams in a room get broadcasts" "ok" "$sse_rooms_out"; fi
check "proc.start/read/wait a real command end to end" GET /proc/echo_full        200 '"out":"hello from proc\n","code":0'
check "proc.start with a bad command is a hard error"  GET /proc/missing_command  500 'proc.start(): could not start'
check "proc.kill + wait reaps a live process"          GET /proc/kill_and_wait    200 '"was_alive":true,"code":'
check "proc.kill + wait leaves it not alive"           GET /proc/kill_and_wait    200 '"still_alive":false'
check "proc.read without stdout: pipe is an error" GET /proc/read_without_pipe 500 '"error":"proc.read(): this process was not started with stdout'
check "proc.alive on an unknown handle errors"         GET /proc/unknown_handle   500 'proc: unknown handle'

echo "== Range support on send_file() (RFC 7233) =="
next_server "$HERE/cases/range.lux" || exit 1
check "rooted send_file serves a file inside root" GET /rooted/range_fixture.bin 200 '0123456789'
check "rooted send_file refuses a name above root"  GET /rooted/..%2F..%2F..      403 'Forbidden'

# check() only inspects status + body; Range needs a request header AND a
# response header (Content-Range/Accept-Ranges) neither of that checks, so
# this is its own small helper -- same "custom curl invocation" shape
# json_utf8() above already uses for the same reason (needing more than
# check() inspects).
#
# The fixture (range_fixture.bin) is a fixed 144 bytes, four repeats of
# "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" -- known content, so every check
# below asserts the actual bytes returned, not just a status code.
check_range() {
    local name="$1" range_header="$2" want_code="$3" header_needle="$4" body_needle="${5:-}"
    local args=(-sS --max-time 10 -D "$TMP/headers" -o "$TMP/body" -w '%{http_code}')
    [ -n "$range_header" ] && args+=(-H "Range: $range_header")
    local got
    got=$(curl "${args[@]}" "http://127.0.0.1:$PORT/range/file" 2>/dev/null)
    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got"
        return
    fi
    if [ -n "$header_needle" ] && ! grep -qiF "$header_needle" "$TMP/headers"; then
        fail "$name" "a header containing '$header_needle'" "$(cat "$TMP/headers")"
        return
    fi
    if [ -n "$body_needle" ] && ! grep -qF "$body_needle" "$TMP/body"; then
        fail "$name" "body to contain '$body_needle'" "$(cat -v "$TMP/body")"
        return
    fi
    ok "$name"
}

check_range "no Range header -- whole file, advertises support" "" 200 "Accept-Ranges: bytes" "0123456789"
check_range "bytes=0-9 -- first 10 bytes"          "bytes=0-9"     206 "Content-Range: bytes 0-9/144"     "0123456789"
check_range "bytes=-10 -- suffix, last 10 bytes"   "bytes=-10"     206 "Content-Range: bytes 134-143/144" "QRSTUVWXYZ"
check_range "bytes=134- -- start to EOF, same 10 bytes" "bytes=134-" 206 "Content-Range: bytes 134-143/144" "QRSTUVWXYZ"
check_range "end past EOF is clamped, not rejected" "bytes=100-99999" 206 "Content-Range: bytes 100-143/144" ""
check_range "start past EOF is 416"                "bytes=99999-100005" 416 "Content-Range: bytes */144" ""
check_range "unparseable Range is ignored -- whole file" "not-a-range" 200 "" "0123456789"
check_range "multi-range is ignored (unsupported) -- whole file" "bytes=0-9,20-29" 200 "" "0123456789"

echo "== static mount at the root (static \"/\" -> ...) =="
next_server "$HERE/cases/static.lux" || exit 1
check "GET / serves index.html"            GET /              200 '<html>index</html>'
check "a real file by its own name"        GET /index.html    200 '<html>index</html>'
check "a real file in a subdirectory"      GET /css/style.css 200 'body{color:red}'
check "another real file at the root"      GET /test.txt      200 'plain file'
check "unknown path falls back to index.html (spa)" GET /whatever/nope 200 '<html>index</html>'

echo "== templates =="
next_server "$HERE/cases/render.lux" || exit 1
check "render() fills a template"   GET /greet 200 '<h1>Hi</h1>'
check "render() escapes its values" GET /greet 200 '<li>Ana &lt;b&gt;</li><li>Bob</li>'
check "render() evaluates in it"    GET /greet 200 '<p>2 names</p>'
check "template: nested loops and loop" GET /shapes 200 '<section n="2/3">g&lt;1&gt;&amp;&quot;&#39;'
check "template: loop.first/last, if/else" GET /shapes 200 '[0,1,2]'
check "template: elif, arithmetic, missing field, list" GET /shapes 200 'small 21 null [&quot;a&quot;,&quot;b&quot;]'
check "template: |safe and include" GET /shapes 200 '<footer>HI &lt;YOU&gt; (3)</footer>'
check "template: a field of a non-Dict" GET /shapes_bad 500 "'name' on int, which has no fields"
check "template: plain reads, nested loops" GET /direct 200 'a:1,2,a;b&lt;:b&lt;;'
check "template: plain reads, a non-Dict item" GET /direct_bad 500 "'name' on int, which has no fields"
check "template: and/or, arithmetic, concatenation" GET /exprs 200 '1<1/2:1><2/2:3>|true|false|-2|#2!|0.5|'
check "template: loop by field and whole" GET /exprs 200 '2<1/1:5>|false|true|-5|#5!|1.25|{&quot;index&quot;:3,&quot;index0&quot;:2,'
check "template: an expression's error" GET /exprs_bad 500 'cannot compare string and int'
check "records: a list of one shape, as JSON" GET /records 200 '[{"name":"a\u003c0\u003e\"é\n","price":0.5,"ok":true,"w":0},{"name":"a\u003c1\u003e\"é\n","price":1.0,"ok":false,"w":1},'
check "records: read inside a return"        GET /records_nested 200 '{"rows":[{"id":0,"tag":"t0"},{"id":1,"tag":"t1"}],"n":2,"first":{"id":0,"tag":"t0"}}'
check "records: fields and loop in a template" GET /records_tpl 200 '1/3:n&lt;0&gt;|n<0>|0|false|0.5;2/3:n&lt;1&gt;|n<1>|2|true|0.5;3/3:n&lt;2&gt;|n<2>|4|false|0.5;'
check "records: a row read as a whole"       GET /records_tpl 200 '[0null][4null][8null]'
check "records: two shapes stay Dicts"       GET /records_mixed 200 '[{"a":1},{"b":2}]'
check "records: a reused slot is not one"   GET /records_slot 200 '{"x":1}'
check "records: ...and its own branch is"    GET '/records_slot?c=1' 200 '[{"a":1}]'

echo "== --native semantics (native_edges.lux) =="
next_server "$HERE/cases/native_edges.lux" /mixed || exit 1
check "a list shared by two variables"      GET /values 200 '"b":[1,2,3]'
check "sort() on a typed list"              GET /values 200 '"c":[1,4,5]'
check "an int summed with range() elements" GET /values 200 '"total":6'
check "int / int: exact or not"             GET /values 200 '"div":3.5,"exact":2'
check "and/or give the winning operand"     GET /values 200 '"and":0,"or":5,"null":null'
check "a function that awaits, with and without await" GET /awaits 200 '"x":4,"y":6'
check "a catch whose body awaits"           GET /awaits 200 '"caught":"division by zero"'
check "a null result is a 204"              GET /nothing 204
check "await on the skipped side of or"     GET /short_circuit 401
check "List methods on typed lists"         GET /list_methods 200 '{"had":true,"at":1,"missing":-1,"gone":true,"out":false,"xs":[3,3,7,9],"ys":[3,3,7,9],"words":["c","b","a"],"part":[3,7,9],"tail":[9,1],"both":[3,7,9,9,1],"joined":"c-b-a","first":3,"last":1,"popped":1,"lo":3,"hi":"c","isum":22,"fsum":4.0,"esum":0,"emin":null,"epop":null}'
check "moves only what is not read again"   GET /moves 200 '{"s":"01234","rows":[{"k":1},{"k":1},{"k":1}],"parts":["ab","ab"],"piece":"ab","b":{"x":1},"a2":{"y":2},"d":{"y":2},"xs":[1,2,3],"pair":[{"k":1},{"k":1}],"caught":"{\"z\":3}","rep":"xyxyxy"}'

echo "== compile errors =="
compiles    "the repo examples compile" "$HERE/cases/language.lux"
compiles    "example/ app compiles"     "$HERE/../example"
native_compiles "--native compiles the sqlite suite"   "$HERE/cases/sqlite.lux"   27
native_compiles "--native compiles the postgres suite" "$HERE/cases/postgres.lux" 22
native_compiles "--native compiles the mysql suite"    "$HERE/cases/mysql.lux"    17
native_compiles "--native compiles module calls"      "$HERE/cases/modules.lux"  76
native_compiles "--native survives its edge cases"    "$HERE/cases/native_edges.lux" 5
native_compiles "--native compiles the class suite"   "$HERE/cases/classes.lux"  13
native_compiles "--native compiles File/List<File> parameters" "$HERE/cases/params.lux" 8
native_compiles "--native compiles session and jwt" "$HERE/cases/session.lux" 5
native_compiles "--native compiles render()"          "$HERE/cases/render.lux"   12
native_compiles "--native compiles the language suite" "$HERE/cases/language.lux" 49
fails_to_compile "pattern without a parameter"  "$HERE/cases/bad/pattern.lux"   "no parameter binds it"
fails_to_compile "missing await"       "$HERE/cases/bad/await.lux"    "is asynchronous"
fails_to_compile "object out of place" "$HERE/cases/bad/sse.lux"      "only exists inside a route sse"
fails_to_compile "ws without origins"        "$HERE/cases/bad/ws.lux"       "needs origins"
fails_to_compile "unknown field"     "$HERE/cases/bad/validate.lux" "is not declared"
fails_to_compile "unknown method"    "$HERE/cases/bad/method.lux"   "has no method"
fails_to_compile "method on a string"   "$HERE/cases/bad/method_type.lux" "have no method"
fails_to_compile "field of a class"    "$HERE/cases/bad/field_type.lux"  "has no field"
fails_to_compile "module not imported"   "$HERE/cases/bad/import.lux"   "missing 'import sqlite'"
fails_to_compile "native module not imported" "$HERE/cases/bad/module_import.lux" "missing 'import hash'"
fails_to_compile "a module's return type is checked" "$HERE/cases/bad/module_return.lux" "have no method 'uppercase'"
fails_to_compile "a bad schedule"        "$HERE/cases/bad/every.lux"    "is not a schedule"
fails_to_compile "'Response' as a return type" "$HERE/cases/bad/response_type.lux" "cannot be used as a return type"
fails_to_compile "backslash-newline in a string" "$HERE/cases/bad/string_break.lux" "use triple quotes"
fails_to_compile "declared type vs initializer" "$HERE/cases/bad/decl_type.lux" "'a' is declared int but is initialized with string"
fails_to_compile "declared type vs assignment"  "$HERE/cases/bad/decl_type.lux" "cannot assign string to 'k', declared int"
fails_to_compile "fn return type"               "$HERE/cases/bad/decl_type.lux" "this fn returns int, not string"
compiles         "declared types: valid cases"  "$HERE/cases/decl_types_ok.lux"
# Declared types (variables, assignments, fn returns) are checked when both
# sides are statically known.  Expression types are checked at RUN TIME:
# the compiler does not verify that `s - 1` adds up.
# That is already covered by the "no coercion" test of the language suite.

# ─── What a real application needed (abort, errors, forms, state, SQL, ...) ─────────
cd "$HERE/cases"
next_server "$HERE/cases/wishlist.lux" /hello || exit 1
check "on start: ran before the first request"  GET /in 200 '"r":[{"name":"n2"}],"e":[]'
check "abort() from a helper, status only"  GET '/abort?who=' 302
check "abort() with a message reaches on error" GET '/abort?who=banned' 403 'Oops 403: banned user'
check "abort() is not catchable by try"     GET '/abort?who=banned' 403 'Oops 403'
check "abort() lets a normal call through"  GET '/abort?who=ana' 200 '"who":"ana"'
check "status(code, message) feeds error.message" GET /msg 409 'Oops 409: already there'
check "render() works inside on error"      GET /nowhere 404 'Oops 404'
check "on error may await (a database read)" GET /teapot 418 'Oops 418: n3'
check "an on error that fails keeps the original response" GET /full 507 '"error":"disk full"'
check "query_list / request.query"          GET '/form?t=1&t=2&z' 200 '"q":["1","2"],"rq":"t=1\u0026t=2\u0026z"'
check "request.scheme without a proxy"      GET /form 200 '"scheme":"http"'
check "state.hit counts a sliding window"   GET /hit 200 '"n":1,"ttl_ok":true'
check "state.hit counts again"              GET /hit 200 '"n":2'
check "a List is a list of SQL parameters"  GET /in 200 '"r":[{"name":"n2"}],"e":[]'
check "sha1, hmac_sha1, base32, totp (RFC vectors)" GET /crypto 200 '"sha1":"a9993e364706816aba3e25717850c26c9cd0d89d","hmac":"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79","b32":"MZXW6YTBOI","totp":"287082","verify_bad":false'
check "argon2 / bcrypt hash and verify"     GET /pw 200 '"a_ok":true,"a_bad":false,"b_ok":true,"b_bad":false,"ref_bcrypt":true'
check "IANA time zones with daylight saving" GET /tz 200 '"summer":"14:00","winter":"13:00","dst_day":"2026-03-29 00:00"'
check "a template calls project fns and modules" GET /tpl 200 '4 KB | 14:00'
check "send_file adds Content-Disposition"  GET /file 200
for h in 'x-wishlist: yes'; do
    curl -sI "http://127.0.0.1:$PORT/hello" | tr -d '\r' | grep -qi "^$h" && ok "app: headers on every response" || fail "app: headers" "$h" "missing"
done
disp=$(curl -sI "http://127.0.0.1:$PORT/file" | tr -d '\r' | grep -i '^content-disposition')
[[ "$disp" == *'attachment; filename="w.lux"'* ]] && ok "send_file Content-Disposition" || fail "send_file Content-Disposition" "attachment; filename=\"w.lux\"" "$disp"
etag=$(curl -sI "http://127.0.0.1:$PORT/file" | tr -d '\r' | grep -i '^etag' | cut -d' ' -f2)
code=$(curl -s -o /dev/null -w '%{http_code}' -H "If-None-Match: $etag" "http://127.0.0.1:$PORT/file")
[ "$code" = 304 ] && ok "send_file answers 304 to If-None-Match" || fail "send_file 304" "304" "$code"
# A body past 16 MB is refused at once by its Content-Length; the cap is configurable.
head -c 20000000 /dev/zero > "$TMP/big.bin"
code=$(curl -s -o /dev/null -w '%{http_code}' --data-binary @"$TMP/big.bin" "http://127.0.0.1:$PORT/form")
[ "$code" = 413 ] && ok "a body over max_body is a 413" || fail "max_body" "413" "$code"
rm -f "$HERE/cases/wishlist-tests.db"*
cd "$HERE/.."

# ─── Summary ─────────────────────────────────────────────────────────────────

summary
