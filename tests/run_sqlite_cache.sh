#!/usr/bin/env bash
#
# The sqlite result cache (src/lux_script/sqlite_result_cache.hpp): a result is reused only while the tables it
# read are unchanged, and it is never shared with the handler that got it.
#
#   tests/run_sqlite_cache.sh [path-to-binary]

PORT=${LUX_TEST_SQLITE_CACHE_PORT:-8830}
source "$(dirname "$0")/lib.sh"
trap 'kill_wait $SRV; rm -rf "$TMP" "$HERE/cases/tests-sqlite-cache.db"*' EXIT

skip_without_module sqlite "$HERE/cases/sqlite_cache.lux"
rm -f "$HERE/cases/tests-sqlite-cache.db"*

cd "$HERE/.."
start_server "$HERE/cases/sqlite_cache.lux" || exit 1
check "schema" GET /setup 200 '"ok":true'

echo "== a repeated read =="
check "first"  GET /item/1 200 '"n":10'
check "again"  GET /item/1 200 '"n":10'
check "another key" GET /item/2 200 '"n":20'

echo "== a write shows at once =="
check "bump"             POST /bump/1 200 '"changed":1'
check "read after write" GET  /item/1 200 '"n":11'
check "bump again"       POST /bump/1 200 '"changed":1'
check "and again"        GET  /item/1 200 '"n":12'
check "the other key is untouched" GET /item/2 200 '"n":20'

echo "== a write to another table =="
check "other, before" GET /other 200 '"v":5'
check "bump other"    POST /bump_other 200 '"ok":true'
check "other, after"  GET /other 200 '"v":6'
check "a join sees both tables" GET /join 200 '"v":6'
check "bump other again" POST /bump_other 200 '"ok":true'
check "the join follows"  GET /join 200 '"v":7'

echo "== the handler's edits stay with it =="
check "the handler sees its own edit" GET /mutate 200 'MUTATED'
check "the next read is the database's" GET /item/1 200 '"name":"one"'
check "and has no extra row" GET /item/1 200 '"n":12'

echo "== transactions =="
check "inside the transaction the write is seen" POST /tx_peek 200 '"inside":999'
check "after the rollback it is not"             POST /tx_peek 200 '"after":12'
check "a later read agrees" GET /item/1 200 '"n":12'

echo "== statements that are not repeatable =="
check "random" GET /random 200 '"r"'
a=$(curl -s "http://127.0.0.1:$PORT/random"); b=$(curl -s "http://127.0.0.1:$PORT/random")
[ "$a" != "$b" ] && ok "random() differs between calls" || fail "random() differs between calls" "two values" "$a / $b"
a=$(curl -s "http://127.0.0.1:$PORT/now"); sleep 0.05; b=$(curl -s "http://127.0.0.1:$PORT/now")
[ "$a" != "$b" ] && ok "now is not frozen" || fail "now is not frozen" "two values" "$a / $b"

echo "== schema changes =="
check "create"     POST /ddl_a 200 '"ok":true'
check "read"       GET  /ddl_read 200 '"a":1'
check "read twice" GET  /ddl_read 200 '"a":1'
check "recreate with another shape" POST /ddl_b 200 '"ok":true'
check "the new columns show" GET /ddl_read 200 '"b":2'

echo "== many writers, one total =="
pids=""
for i in $(seq 1 120); do curl -s -X POST "http://127.0.0.1:$PORT/bump/2" > /dev/null & pids="$pids $!"; done
# readers in between: whatever they cached must not outlive the last write
for i in $(seq 1 60); do curl -s "http://127.0.0.1:$PORT/item/2" > /dev/null & pids="$pids $!"; done
for p in $pids; do wait "$p" 2>/dev/null; done
check "the final read has every write" GET /item/2 200 '"n":140'

echo "== rows =="
check "a repeated column name keeps the last value" GET /dupcols 200 '"a":2'
check "and the others" GET /dupcols 200 '"b":3'

echo "== a write from outside =="
check "before" GET /item/1 200 '"n":12'
sqlite3 "$HERE/cases/tests-sqlite-cache.db" "update items set n = 77 where id = 1" 2>/dev/null
sleep 0.6   # result_cache_ms is 400 in this app
check "after the time limit it is seen" GET /item/1 200 '"n":77'

kill -0 "$SRV" 2>/dev/null && ok "the server is still alive" || fail "the server is still alive" "alive" "dead"
summary
