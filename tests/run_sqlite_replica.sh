#!/usr/bin/env bash
#
# The sqlite writer (group commit) and `replicate`.
#
#   - concurrent writes, some failing, in the same batches: each gets its own
#     result and id, and reads its own write back at once;
#   - two replicas (a directory, a file:// URL) rebuilt with `lux restore`
#     match the live database: after a checkpoint resets the WAL, after a
#     crash (kill -9, what was shipped survives), after a clean stop (the last
#     commits are flushed), after someone else resets the WAL;
#   - only the current generation and the one before are kept.
#
#   tests/run_sqlite_replica.sh [path-to-binary]

PORT=${LUX_TEST_SQLITE_REPLICA_PORT:-8821}
source "$(dirname "$0")/lib.sh"

export REPL_DB="$TMP/app.db" REPL_A="$TMP/replica-a" REPL_B="file://$TMP/replica-b"
skip_without_module sqlite "$HERE/cases/sqlite_replica.lux"

db_stats()   { python3 -c "import sqlite3,sys; print('%d %d' % sqlite3.connect(sys.argv[1]).execute('select count(*), coalesce(sum(n),0) from items').fetchone())" "$1"; }
live_stats() { curl -s "http://127.0.0.1:$PORT/stats" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['c'], d['s'])"; }

# restored_matches <name> <replica> <expected "count sum">
restored_matches() {
    local out="$TMP/restored-$RANDOM.db" got
    if ! "$LUX" restore "$2" "$out" > "$TMP/restore.log" 2>&1; then
        fail "$1" "a restore" "$(cat "$TMP/restore.log")"; return
    fi
    got=$(db_stats "$out")
    if [ "$got" = "$3" ]; then ok "$1"; else fail "$1" "$3" "$got ($(cat "$TMP/restore.log"))"; fi
}

graceful_stop() { kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""; }

echo "== the writer =="
start_server "$HERE/cases/sqlite_replica.lux" || exit 1
check "creates the schema" GET /create 200 '"ok":true'

# 200 at once; n repeats for 20 of them, so the unique index rejects those
# inside batches where the others commit.
pids=""
for i in $(seq 1 200); do
    n=$(( i <= 180 ? i : i - 180 ))
    curl -s -X POST -o "$TMP/w$i" -w '%{http_code}' "http://127.0.0.1:$PORT/add/$n" > "$TMP/c$i" & pids="$pids $!"
done
for p in $pids; do wait "$p"; done
created=0; conflicts=0; wrong=0
for i in $(seq 1 200); do
    n=$(( i <= 180 ? i : i - 180 ))
    case "$(cat "$TMP/c$i")" in
        201) created=$((created + 1)); grep -q "\"n\":$n}" "$TMP/w$i" || wrong=$((wrong + 1)) ;;
        409) conflicts=$((conflicts + 1)) ;;
        *)   wrong=$((wrong + 1)) ;;
    esac
done
[ "$created" = 180 ] && [ "$conflicts" = 20 ] && ok "180 created, 20 rejected by the unique index" \
    || fail "180 created, 20 rejected by the unique index" "180/20" "$created/$conflicts"
[ "$wrong" = 0 ] && ok "each write read back its own row by its own last_id" \
    || fail "each write read back its own row by its own last_id" "0 wrong" "$wrong wrong"
ids=$(cat "$TMP"/w* | grep -o '"id":[0-9]*' | sort | uniq -d | wc -l)
[ "$ids" = 0 ] && ok "no two writes got the same id" || fail "no two writes got the same id" "0" "$ids repeated"
check "the table holds exactly those" GET /stats 200 '"c":180'

# 40 transactions at once, each pausing between its two inserts.
pids=""
for i in $(seq 1 40); do
    curl -s -X POST -o "$TMP/t$i" -w '%{http_code}' "http://127.0.0.1:$PORT/tx/$((500 + i))" > "$TMP/tc$i" & pids="$pids $!"
done
for p in $pids; do wait "$p"; done
bad=0
for i in $(seq 1 40); do [ "$(cat "$TMP/tc$i")" = 201 ] || bad=$((bad + 1)); done
[ "$bad" = 0 ] && ok "40 overlapping transactions all commit" \
    || fail "40 overlapping transactions all commit" "40 x 201" "$bad failed: $(cat "$TMP"/t* | grep -m1 error)"
check "each one whole: two rows apiece" GET /stats 200 '"c":260'

echo "== replication =="
check "2500 commits (past a checkpoint and a WAL reset)" POST /bulk/1000/2500 200
check "and more after it" POST /bulk/5000/300 200
sleep 2.5
want=$(live_stats)
restored_matches "a directory replica restores to the live data"   "$REPL_A" "$want"
restored_matches "a file:// replica restores to the live data"     "$REPL_B" "$want"

echo "== someone else checkpoints =="
# Right after a write, before it ships: another process's checkpoint must not
# get to reset the WAL under the replica (the pin holds it off).
check "writes" POST /bulk/9000/50 200
busy=$(python3 -c "import sqlite3,sys; print(sqlite3.connect(sys.argv[1], timeout=0.5).execute('pragma wal_checkpoint(truncate)').fetchone()[0])" "$REPL_DB")
[ "$busy" = 1 ] && ok "another process cannot checkpoint past the replica" \
    || fail "another process cannot checkpoint past the replica" "busy=1" "busy=$busy"
check "writes after it" POST /bulk/9100/50 200
sleep 2.5
restored_matches "and the replica has everything" "$REPL_A" "$(live_stats)"

echo "== crash =="
check "writes" POST /bulk/20000/100 200
sleep 2.5
want=$(live_stats)
stop_server   # kill -9
restored_matches "what was shipped before a kill -9 survives" "$REPL_A" "$want"

echo "== clean stop =="
start_server "$HERE/cases/sqlite_replica.lux" || exit 1
check "writes right before stopping" POST /bulk/30000/100 200
want=$(live_stats)
graceful_stop
restored_matches "a clean stop ships the last commits" "$REPL_A" "$want"
restored_matches "to every replica"                    "$REPL_B" "$want"

echo "== generations =="
start_server "$HERE/cases/sqlite_replica.lux" || exit 1
check "writes" POST /bulk/40000/10 200
graceful_stop
gens=$(ls "$REPL_A" | wc -l)
[ "$gens" -le 2 ] && ok "old generations are pruned ($gens kept)" || fail "old generations are pruned" "<= 2" "$gens"

"$LUX" restore "$REPL_A" "$TMP/x.db" > /dev/null 2>&1
"$LUX" restore "$REPL_A" "$TMP/x.db" > "$TMP/restore.log" 2>&1 \
    && fail "restore refuses to overwrite a file" "an error" "it overwrote" \
    || ok "restore refuses to overwrite a file"

summary
