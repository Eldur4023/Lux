#!/usr/bin/env bash
#
# Suite for Lux's `pdf` native module (NATIVE-MODULES.md).
#
# Separate from run_tests.sh for the same reason run_sqlite.sh/run_postgres.sh/
# run_mysql.sh are: cairo is an OPTIONAL compiled-in dependency (LUX_PDF),
# and a binary built without it has to skip this suite, not fail it.
#
#   tests/run_pdf.sh [path-to-binary]

PORT=${LUX_TEST_PDF_PORT:-8830}
source "$(dirname "$0")/lib.sh"

skip_without_module pdf "$HERE/cases/pdf.lux"

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
start_server "$HERE/cases/pdf.lux" /health || exit 1
ok "starts and connects"

echo "== generation =="
check "single page"   GET /make  200 '"len"'
check "multi-page"    GET /pages 200 '"len"'
check_real_pdf "decodes to a real PDF (magic bytes)" /make

echo "== error paths =="
check "unknown handle"        GET /unknown_handle    500 'unknown handle'
check "draw after finish"     GET /draw_after_finish 500 'already saved'
check "close twice"           GET /close_twice       200 '"first":true,"second":false'

summary
