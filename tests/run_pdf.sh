#!/usr/bin/env bash
#
# Suite for Lumen's `pdf` native module (NATIVE-MODULES.md).
#
# Separate from run_tests.sh for the same reason run_sqlite.sh/run_postgres.sh/
# run_mysql.sh are: cairo is an OPTIONAL compiled-in dependency (LUMEN_PDF),
# and a binary built without it has to skip this suite, not fail it.
#
#   tests/run_pdf.sh [path-to-binary]

set -u

LUMEN="${1:-$HOME/lumen-build/lumen}"
case "$LUMEN" in /*) ;; *) LUMEN="$(cd "$(dirname "$LUMEN")" && pwd)/$(basename "$LUMEN")" ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
PORT=${LUMEN_TEST_PDF_PORT:-8830}
SRV=""

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

stop_server() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
    SRV=""
}
trap 'stop_server; rm -rf "$TMP"' EXIT

if ! "$LUMEN" --check "$HERE/cases/pdf.lum" > "$TMP/check" 2>&1; then
    if grep -q "pdf" "$TMP/check" && grep -q "import" "$TMP/check"; then
        grey "pdf: the binary was built without the module — suite skipped"
        exit 77
    fi
    red "the test file does not compile:"; cat "$TMP/check"; exit 1
fi

check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}"
    local got
    got=$(curl -sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}' \
          "http://127.0.0.1:$PORT$path" 2>/dev/null)
    local body; body=$(head -c 400 "$TMP/body" 2>/dev/null)
    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"; return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"; return
    fi
    ok "$name"
}

# A real, valid PDF, not just a non-empty response: decodes the base64 body
# straight from the JSON response and checks the magic bytes are really
# there -- the same discipline the module's manual verification used
# (NATIVE-MODULES.md), kept in the suite instead of a one-off check.
check_real_pdf() {
    local name="$1" path="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PORT$path" 2>/dev/null
    local magic
    magic=$(python3 -c "
import json, base64
d = json.load(open('$TMP/body'))
raw = base64.b64decode(d['b64'])
print(raw[:5].decode('latin1'), end='')
" 2>/dev/null)
    if [ "$magic" = "%PDF-" ]; then
        ok "$name"
    else
        fail "$name" "decoded bytes starting with %PDF-" "$(head -c 200 "$TMP/body")"
    fi
}

echo "== startup =="
cd "$HERE/.."
"$LUMEN" --no-watch --port "$PORT" "$HERE/cases/pdf.lum" > "$TMP/srv.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/health" 2>/dev/null && break
    kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; exit 1; }
    sleep 0.3
done
curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/health" || {
    red "the server did not answer"; cat "$TMP/srv.log"; exit 1; }
ok "starts and connects"

echo "== generation =="
check "single page"   GET /make  200 '"len"'
check "multi-page"    GET /pages 200 '"len"'
check_real_pdf "decodes to a real PDF (magic bytes)" /make

echo "== error paths =="
check "unknown handle"        GET /unknown_handle    500 'unknown handle'
check "draw after finish"     GET /draw_after_finish 500 'already saved'
check "close twice"           GET /close_twice       200 '"first":true,"second":false'

echo
if [ "$failed" -eq 0 ]; then
    green "$passed tests, all passing"
    exit 0
fi
red "$passed passed, $failed failed"
exit 1
