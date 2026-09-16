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

set -u

LUX="${1:-$HOME/lux-build/lux}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUX_TEST_PORT:-8790}
SRV=""
# For the `os` module's file-I/O tests (cases/modules.lux): a writable
# scratch dir the server can see via os.getenv(), cleaned up by the same
# trap that removes $TMP itself -- no test-only directory left behind.
export LUX_TEST_TMP="$TMP"

passed=0
failed=0

# ─── Helpers ─────────────────────────────────────────────────────────────────

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

ok()   { passed=$((passed + 1)); printf '  ok    %s\n' "$1"; }
fail() {
    failed=$((failed + 1))
    red "  FAIL $1"
    printf '        expected: %s\n        got: %s\n' "$2" "$3"
}

# Starts a server with the given .lux and waits for it to answer.
# Each suite uses its own port: with SO_REUSEPORT two processes share a
# port and the kernel splits connections between them, which would skew everything.
start_server() {
    stop_server
    PORT=$((PORT + 1))
    "$LUX" --no-watch --port "$PORT" "$1" > "$TMP/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/__ping__" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; return 1; }
        sleep 0.2
    done
    red "the server did not answer"; cat "$TMP/srv.log"; return 1
}

stop_server() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    # Wait ONLY for that pid: a bare `wait` would also wait for the
    # server and hang the suite.
    wait "$SRV" 2>/dev/null
    SRV=""
}

# check <name> <method> <path> <expected-code> [expected-substring] [data]
check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}" data="${6:-}"
    local args=(-sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}')
    [ -n "$data" ] && args+=(-H 'Content-Type: application/json' -d "$data")

    local got
    got=$(curl "${args[@]}" "http://127.0.0.1:$PORT$path" 2>/dev/null)
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

cleanup() { stop_server; rm -rf "$TMP"; }
trap cleanup EXIT

# ─── Suites ──────────────────────────────────────────────────────────────────

echo "== language =="
start_server "$HERE/cases/language.lux" || exit 1
check "arithmetic"          GET /arithmetic       200 '"sum":7'
check "strings"             GET /strings          200 '"upper":"HELLO"'
check "multiline without margin" GET /multiline     200 '"sql":"SELECT id\nFROM posts"'
check "one-line multiline"   GET /multiline       200 '"loose":"on one line"'
check "multiline escapes"  GET /multiline       200 '"escapes":"with \"quotes\""'
check "truthiness"           GET /truthiness        200 '"zero":false'

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

check "range(n)"              GET /range 200 '"one_arg":[0,1,2,3,4]'
check "range(start, end)"     GET /range 200 '"two_args":[2,3,4,5]'
check "range with step"       GET /range 200 '"step":[0,2,4,6,8]'
check "range with negative step" GET /range 200 '"negative_step":[5,4,3,2,1]'
check "range() in a for loop" GET /range_for_loop 200 '"total":10'
check "range() rejects step 0" GET /range_bad_step 500 'step cannot be 0'

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
start_server "$HERE/cases/routes.lux" || exit 1
check "path parameter"   GET /echo/42           200 '"id":42'
check "query with default"   GET /pagina           200 '"page":1'
check "query explicita"     GET '/pagina?page=7'  200 '"page":7'
check "invalid type"       GET /echo/abc          400 'invalid parameter'
check "path inexistente"    GET /nada             404 'not found'
check "group with prefix"   GET /api/v1/hello      200 '"v":1'
check "guard denies"      GET /admin/panel      403
check "guard allows"      GET '/admin/panel?k=abre' 200 '"panel":true'
check "404 handler"       GET /tampoco          404 '"path":"/tampoco"'

# ── Parameter binding matrix ──────────────────────────────────────────────
# Origen (path / query / multipart) x type (escalar, File, List<File>) x
# presence (missing, mistyped, in two places at once).  See cases/params.lux:
# the real bug was a text parameter ALWAYS empty on a route with files,
# and it only shows up when the two live on the same route -- testing query and
# multipart separately, as the rest of the suite did, never caught it.
echo "== parameter binding =="
start_server "$HERE/cases/params.lux" || exit 1

check "missing query without a default gives the type's zero" GET /query 200 '"q":""'

check_mp "multipart: text next to a file" /mp/one 200 '"title":"hello"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "title=hello"
check_mp "multipart: the file arrives too" /mp/one 200 '"filename":"a.txt"' \
    -F "f=@$HERE/cases/params.lux;filename=a.txt" -F "title=hello"

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
start_server "$HERE/cases/classes.lux" || exit 1
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

echo "== session and jwt =="
start_server "$HERE/cases/session.lux" || exit 1
check "no session"          GET /quien            200 '"user":null'
check "protected area"      GET /admin/panel      403
check "forged cookie"  GET /quien            200 '"user":null'
check "jwt missing"         GET /api/yo           401
check "invalid jwt"        GET /api/yo           401

echo "== database =="
rm -f "$TMP/tests.db"
start_server "$HERE/cases/data.lux" || exit 1
check "create table"         GET  /create           200 '"ok":true'
check "insert"            POST /add/ana        201 '"id":1'
check "insert another"       POST /add/bob        201 '"id":2'
check "list"              GET  /all           200 '"name":"ana"'
check "lookup by id"       GET  /one/1           200 '"name":"ana"'
check "no encontrado"       GET  /one/99          404
check "sql injection"       GET  "/search?q=ana'%20OR%20'1'='1" 200 '"encontrados":0'
check "transaction"         POST /transfer      200 '"ok":true'
check "balances after commit"  GET  /balances          200 '"balance":70'
check "rollback"            POST /undo         200 '"deshecho":true'
check "balances after rollback" GET /balances          200 '"balance":70'
check "engine error"     GET  /bad            200 'no such table'

echo "== native modules =="
start_server "$HERE/cases/modules.lux" || exit 1
check "hash.sha256"       GET /hash/test              200 '"sha256":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"'
check "hash.hmac_sha256"  GET /hmac/mykey/mymsg       200 '"mac":"131b7d67f083953569aa6d777328a10cf618900e9d261397d750ec30df3c9654"'
check "hash.random_hex length" GET /random/8         200
random_hex_len=$(curl -sS --max-time 10 "http://127.0.0.1:$PORT/random/8" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['hex']))")
if [ "$random_hex_len" = "16" ]; then ok "hash.random_hex(8) is 16 hex chars"
else fail "hash.random_hex(8) is 16 hex chars" "16" "$random_hex_len"; fi
check "csv.parse/columns/rows"     GET /csv/basic                 200 '"name":"ana","age":30,"city":"madrid"'
check "csv row_count"              GET /csv/basic                 200 '"row_count":3'
check "csv.filter_eq"              GET /csv/filter_and_aggregate  200 '"name":"cleo","age":35,"city":"madrid"'
check "csv.sum/mean"               GET /csv/filter_and_aggregate  200 '"total_age":90.0,"avg_age":30.0'
check "csv.group_sum"              GET /csv/filter_and_aggregate  200 '"group":"paris","sum":25.0'
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
check "os.run missing command errors" GET /os/run_missing         200 '"error":"os.run(): could not start'

check "math.abs/min/max"           GET /math/basic  200 '"abs":7,"abs_f":2.5,"min":3,"max":9'
check "math.round/floor/ceil"      GET /math/basic  200 '"round":3,"floor":2,"ceil":3'
check "math.sqrt/pow/log"          GET /math/basic  200 '"sqrt":4.0,"pow":1024.0,"log":0.0'
check "math.random is in [0,1)"    GET /math/random 200 '"in_range":true'
check "math.random_int inclusive edge" GET /math/random 200 '"fixed_range":5'
check "time.now/now_seconds"       GET /time/basic  200 '"n_positive":true,"s_positive":true'
check "time.format_iso/format epoch0" GET /time/basic 200 '"iso_epoch0":"1970-01-01T00:00:00Z","custom_epoch0":"1970-01-01"'
check "time.parse_iso roundtrip"   GET /time/parse  200 '"parsed_epoch0":0'
check "time.parse_iso invalid is null" GET /time/parse 200 '"bad_is_null":true'

check "regex.test"             GET /regex/test  200 '"yes":true,"no":false'
check "regex.find"             GET /regex/find  200 '"found":"123","missing":null'
check "regex.find_all"         GET /regex/find_all 200 '"all":["1","22","333"]'
check "regex.groups"           GET /regex/groups 200 '"g":["bob@example.com","bob","example"]'
check "regex.replace"          GET /regex/replace 200 '"r":"a# b# c#"'
check "regex.replace backreferences" GET /regex/replace_backref 200 '"r":"host@user"'
check "regex.split"            GET /regex/split 200 '"r":["a","b","c","d"]'
check "regex rejects an invalid pattern" GET /regex/bad_pattern 500 'invalid regex pattern'

check "rooms.count on an unknown room is 0, not an error" GET /rooms/count_empty     200 '"n":0'
check "rooms.broadcast to an unknown room reaches nobody" GET /rooms/broadcast_empty 200 '"reached":0'
check "rooms.join outside a ws route is rejected"         GET /rooms/join_outside_ws 500 'can only be called from a ws route'

check "proc.start/read/wait a real command end to end" GET /proc/echo_full        200 '"out":"hello from proc\n","code":0'
check "proc.start with a bad command is a hard error"  GET /proc/missing_command  500 'proc.start(): could not start'
check "proc.kill + wait reaps a live process"          GET /proc/kill_and_wait    200 '"was_alive":true,"code":'
check "proc.kill + wait leaves it not alive"           GET /proc/kill_and_wait    200 '"still_alive":false'
check "proc.read without stdout: pipe is a soft error" GET /proc/read_without_pipe 200 '"error":"proc.read(): this process was not started with stdout'
check "proc.alive on an unknown handle errors"         GET /proc/unknown_handle   500 'proc: unknown handle'

echo "== compile errors =="
compiles    "the repo examples compile" "$HERE/cases/language.lux"
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
# Expression types are checked at RUN TIME: the compiler
# it verifies names, arity, context, and the methods and fields of a receiver
# whose type it knows -- but not that `s - 1` adds up.
# That is already covered by the "no coercion" test of the language suite.

# ─── Summary ─────────────────────────────────────────────────────────────────

echo
if [ "$failed" -eq 0 ]; then
    green "$passed tests, all passing"
    exit 0
fi
red "$passed passed, $failed failed"
exit 1
