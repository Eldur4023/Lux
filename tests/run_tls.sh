#!/usr/bin/env bash
# HTTPS in the server itself (tls: cert / key). Needs a binary built with
# -DLUX_TLS=ON, skipped (77) otherwise. Usage: tests/run_tls.sh [lux-binary]
set -u
cd "$(dirname "$0")/.."
LUX=${1:-build/lux}; PORT=${TLS_TEST_PORT:-18093}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
mkdir "$T/public"; head -c 3000000 /dev/urandom | base64 -w0 > "$T/public/big.txt"   # 4 MB, more than a socket buffer
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost \
    -keyout "$T/k.pem" -out "$T/c.pem" 2>/dev/null
cat > "$T/app.lux" <<LUX
app:
    port     $PORT
    host     "127.0.0.1"
    static "/static" -> "$T/public"

tls:
    cert "$T/c.pem"
    key  "$T/k.pem"

get endpoint("/hi"):
    return { "scheme": request.scheme }

get endpoint("/big"):
    return "0123456789abcdef".repeat(20000)

post endpoint("/echo"):
    return { "len": len(request.body) }
LUX
"$LUX" --no-watch "$T/app.lux" >"$T/srv.log" 2>&1 & SRV=$!
for _ in $(seq 40); do curl -sk -m 1 -o /dev/null https://127.0.0.1:$PORT/hi && break; sleep 0.2; done
if grep -q "built without HTTPS" "$T/srv.log"; then echo "https: the binary was built without LUX_TLS — suite skipped"; exit 77; fi
fail=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: wanted [$3] got [$2]"; fail=1; fi; }
U=https://127.0.0.1:$PORT
check "request.scheme is https"           "$(curl -sk $U/hi)" '{"scheme":"https"}'
check "TLS 1.2 works"                     "$(curl -sk --tls-max 1.2 -o /dev/null -w '%{http_code}' $U/hi)" 200
check "response larger than one record"   "$(curl -sk $U/big | wc -c)" 320002
check "static file (4 MB) is byte-identical" "$(curl -sk $U/static/big.txt | cmp - "$T/public/big.txt" && echo same)" same
check "static file, slow reader (backpressure)" "$(curl -sk --limit-rate 4M $U/static/big.txt | cmp - "$T/public/big.txt" && echo same)" same
check "Range over TLS"                    "$(curl -sk -r 100-199 $U/static/big.txt | cmp - <(tail -c +101 "$T/public/big.txt" | head -c 100) && echo same)" same
check "2 MB POST body"                    "$(head -c 2000000 /dev/zero | curl -sk --data-binary @- -H 'Content-Type: text/plain' $U/echo)" '{"len":2000000}'
check "keep-alive reuses the connection"  "$(curl -sk -o /dev/null $U/hi -o /dev/null $U/hi -w '%{num_connects}')" 10   # new, then reused
# resumption works with the session cache off: it is the stateless ticket that resumes
# (the ticket comes after the handshake: the connection has to stay open to receive it)
sleep 0.5 | openssl s_client -connect 127.0.0.1:$PORT -tls1_3 -sess_out "$T/sess" >/dev/null 2>&1
check "TLS 1.3 ticket resumes the session" "$(sleep 0.5 | openssl s_client -connect 127.0.0.1:$PORT -tls1_3 -sess_in "$T/sess" 2>/dev/null | grep -c '^Reused')" 1
check "plain HTTP to the TLS port is refused" "$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/hi)" 000
printf '\x16\x03\x01\x00\x10garbage' | timeout 1 nc 127.0.0.1 $PORT >/dev/null 2>&1
check "survives a broken handshake"       "$(curl -sk $U/hi)" '{"scheme":"https"}'
# a key that does not exist is a startup error, not a server that silently speaks HTTP
sed -i "s#k.pem#nokey.pem#" "$T/app.lux"; sed -i "s#port     $PORT#port     $((PORT+1))#" "$T/app.lux"
timeout 5 "$LUX" --no-watch "$T/app.lux" >"$T/bad.log" 2>&1; rc=$?
check "missing tls key stops the server"  "$rc:$(grep -c 'https: ' "$T/bad.log")" "1:1"
exit $fail
