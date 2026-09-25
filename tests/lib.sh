# Shared helpers for the tests/run_*.sh suites. Source it after setting
# PORT; the first argument of the suite is the path to the binary.

set -u

LUX="${1:-$HOME/lux-build/lux}"
# Absolute: several suites cd to the repo root so relative paths in the .lux resolve.
case "$LUX" in /*) ;; *) LUX="$(cd "$(dirname "$LUX")" && pwd)/$(basename "$LUX")" ;; esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
SRV=""
EXTRA_PIDS=""   # other background processes to kill on exit

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

# kill_wait <pid>...: only those pids -- a bare `wait` would also wait for
# the server and hang the suite.
kill_wait() {
    local pid
    for pid in "$@"; do kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; done
}
stop_server() { kill_wait $SRV; SRV=""; }
trap 'kill_wait $SRV $EXTRA_PIDS; rm -rf "$TMP"' EXIT

# start_server <file.lux> [ping-path]: starts it on $PORT and waits for it to
# answer. LUX_TEST_FLAGS (e.g. --native) is passed through to every server.
# Each suite uses its own port: with SO_REUSEPORT two processes
# share a port and the kernel splits connections between them.
start_server() {
    stop_server
    "$LUX" --no-watch ${LUX_TEST_FLAGS:-} --port "$PORT" "$1" > "$TMP/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT${2:-/__ping__}" 2>/dev/null && return 0
        kill -0 "$SRV" 2>/dev/null || { red "the server died on startup:"; cat "$TMP/srv.log"; return 1; }
        sleep 0.2
    done
    red "the server did not answer"; cat "$TMP/srv.log"; return 1
}

# skip_without_module <module> <file.lux>: exits 77 (CMake's "skipped") when
# the binary was built without the module, 1 when the file does not compile.
skip_without_module() {
    "$LUX" --check "$2" > "$TMP/check" 2>&1 && return 0
    if grep -q "module '$1' is not compiled into this binary" "$TMP/check"; then
        grey "$1: the binary was built without the module — suite skipped"
        exit 77
    fi
    red "the test file does not compile:"; cat "$TMP/check"; exit 1
}

# check <name> <method> <path> <expected-code> [expected-substring] [json-data]
check() {
    local name="$1" method="$2" path="$3" want_code="$4" needle="${5:-}" data="${6:-}"
    local args=(-sS --max-time 10 -X "$method" -o "$TMP/body" -w '%{http_code}')
    [ -n "$data" ] && args+=(-H 'Content-Type: application/json' -d "$data")
    local got body
    got=$(curl "${args[@]}" "http://127.0.0.1:$PORT$path" 2>/dev/null)
    body=$(head -c 600 "$TMP/body" 2>/dev/null)
    if [ "$got" != "$want_code" ]; then
        fail "$name" "code $want_code" "code $got — $body"; return
    fi
    if [ -n "$needle" ] && ! grep -qF "$needle" "$TMP/body"; then
        fail "$name" "to contain '$needle'" "$body"; return
    fi
    ok "$name"
}

# json_valid <name> <path>: the body is really valid JSON, UTF-8 included --
# a stray blob byte or a NaN/Infinity would come out in a shape no client reads.
json_valid() {
    local name="$1" path="$2"
    curl -sS --max-time 10 -o "$TMP/body" "http://127.0.0.1:$PORT$path" 2>/dev/null
    if python3 -c "import json,sys; json.load(open(sys.argv[1], encoding='utf-8'))" \
            "$TMP/body" 2>/dev/null; then
        ok "$name"
    else
        fail "$name" "valid UTF-8 JSON" "$(head -c 200 "$TMP/body" | cat -v)"
    fi
}

summary() {
    echo
    if [ "$failed" -eq 0 ]; then
        green "$passed tests, all passing"
        exit 0
    fi
    red "$passed passed, $failed failed"
    [ -s "$TMP/srv.log" ] && { echo "--- server log ---"; tail -30 "$TMP/srv.log"; }
    exit 1
}
