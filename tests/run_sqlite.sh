#!/usr/bin/env bash
#
# Type battery for Lux's sqlite module.
#
# It is separate from run_tests.sh —which already drives sqlite in normal use—
# because this is another thing: the type limits, what sqlite really stores
# versus what the column declaration says, and the statement cache.
#
# It needs no server and no client: the schema is created by the .lux itself, so
# the suite depends only on the binary and always runs.
#
#   tests/run_sqlite.sh [path-to-binary]

PORT=${LUX_TEST_SQLITE_PORT:-8820}
source "$(dirname "$0")/lib.sh"
trap 'kill_wait $SRV; rm -rf "$TMP" "$HERE/cases/tests-sqlite-types.db"*' EXIT

skip_without_module sqlite "$HERE/cases/sqlite.lux"

rm -f "$HERE/cases/tests-sqlite-types.db"*

echo "== startup =="
cd "$HERE/.."
start_server "$HERE/cases/sqlite.lux" || exit 1
ok "starts and connects"
check "creates the schema" GET /create 200 '"ok":true'

echo "== reading =="
check "select without parameters" GET /all      200 '"title":"long"'
check "select with a parameter"  GET /one/1      200 '"author":"Ana"'
check "two reads at once"  GET /pair/1     200 '{"title":"long","count":'
check "missing row"    GET /one/99999  404

echo "== long text =="
check "4000-byte text" GET /length 200 '"received":4000'

echo "== types =="
check "integer maximum"  GET /types 200 '"t_int":9223372036854775807'
check "integer minimum"  GET /types 200 '"t_neg":-9223372036854775808'
check "decimal"        GET /types 200 '"t_real":3.141592653589793'
check "utf8 text"    GET /types 200 'emoji'
check "null"           GET /types 200 '"t_null":null'
# The blob carries x'00FF80FE': bytes that are not valid UTF-8.  They come out in base64.
json_valid "a blob with stray bytes does not break the JSON" /types
check "the blob comes out in base64" GET /types 200 '"t_blob":"AP+A/g=="'

echo "== null byte inside a text =="
check "the driver returns the 5 bytes" GET /null_in_text 200 '"bytes":5'
# The engine's length() counts up to the first NUL: that is its definition, not a fault.
check "sqlite's length() stops at the null" GET /null_in_text 200 '"up_to_null":1'
json_valid "and the JSON stays valid" /null_in_text

echo "== type affinity =="
# sqlite does not enforce the declared type: an integer column can hold text, and
# what comes out has to be what IS THERE, not what the declaration says.
check "text in an integer column" GET /affinity 200 '"type":"text"'
check "integer in the same one"       GET /affinity 200 '"type":"integer"'

echo "== infinite =="
json_valid "an infinity does not break the JSON" /infinite

echo "== statement cache =="
# The same query is prepared once and reused: if the bindings were not cleared
# on reuse, the second call would return the result of the first
# one.
check "first time"       GET /repeated/1 200 '"title":"long"'
check "second, another id"  GET /repeated/2 200 '"title":"short"'
check "third, the first one again" GET /repeated/1 200 '"title":"long"'
check "fourth, missing id"     GET /repeated/9999 200 '"title":null'
check "same query with null"      GET /optional 200 '"n":0'
check "same query with a value"     GET '/optional?author=Ana' 200 '"n":1'

echo "== unicode and injection =="
check "unicode in the bind"      GET /unicode 200 'unicode'
check "quote in the parameter" GET "/injection?q=x'%20OR%20'1'='1" 200 '"found":0'

echo "== writing =="
check "insert and last_id" POST /add/probing 201 '"id"'
check "delete"           POST /delete_row/99999   200 '"rows":0'

echo "== transactions =="
check "commit"               POST /transfer       200 '"ok":true'
check "balances after commit"   GET  /balances           200 '"balance":130'
check "begin without error"      POST /undo_verbose  200 '"begin":true'
check "balances after rollback" GET  /balances           200 '"balance":70'

echo "== errors =="
check "missing table" GET /bad_table          200 "no_existe"
check "too few parameters" GET /too_few_params 200 "were passed"

echo "== concurrency (pool 8) =="
# The statement cache is per worker.  Here it is checked that N workers using
# the SAME query with different parameters do not tread on each other.
conc_failures=0
pids=""
for i in $(seq 1 40); do
    curl -s --max-time 10 -o "$TMP/c$i" -w '%{http_code}' \
      "http://127.0.0.1:$PORT/repeated/$(( (i % 3) + 1 ))" > "$TMP/s$i" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null; done
for i in $(seq 1 40); do
    [ "$(cat "$TMP/s$i" 2>/dev/null)" = "200" ] || conc_failures=$((conc_failures + 1))
    expected=$(( (i % 3) + 1 ))
    case $expected in
        1) grep -q 'long'   "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
        2) grep -q 'short'   "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
        3) grep -q 'unicode' "$TMP/c$i" || conc_failures=$((conc_failures + 1)) ;;
    esac
done
if [ "$conc_failures" -eq 0 ]; then ok "40 concurrent, each with its own result"
else fail "40 concurrent, each with its own result" "40 correct" "$conc_failures wrong"; fi

kill -0 "$SRV" 2>/dev/null && ok "the server is still alive" \
                           || fail "the server is still alive" "alive" "dead"

summary
