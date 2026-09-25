#!/usr/bin/env bash
#
# The optional modules on system libraries: gzip (zlib), crypto (OpenSSL
# libcrypto), image (libjpeg/libpng/libwebp). Skipped on a binary built
# without any of them.
#
#   tests/run_extras.sh [path-to-binary]

PORT=${LUX_TEST_EXTRAS_PORT:-8830}
source "$(dirname "$0")/lib.sh"

"$LUX" --check "$HERE/cases/extras.lux" > "$TMP/check" 2>&1 || {
    grep -q "is not compiled into this binary" "$TMP/check" &&
        { grey "extras: a module is missing from this binary — suite skipped"; exit 77; }
    red "the test file does not compile:"; cat "$TMP/check"; exit 1; }

# Keys made by the openssl CLI, and a JWKS for the RSA one built by hand:
# what Lux signs, openssl verifies, and the other way around.
export LUX_TEST_TMP="$TMP"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/rsa.key" 2>/dev/null
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/ec.key" 2>/dev/null
openssl genpkey -algorithm ED25519 -out "$TMP/ed.key" 2>/dev/null
for k in rsa ec ed; do openssl pkey -in "$TMP/$k.key" -pubout -out "$TMP/$k.pub" 2>/dev/null; done
python3 - "$TMP" <<'PY'
import base64, json, subprocess, sys
d = sys.argv[1]
mod = subprocess.run(["openssl", "rsa", "-pubin", "-in", f"{d}/rsa.pub", "-noout", "-modulus"],
                     capture_output=True, text=True).stdout.strip().split("=")[1]
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
json.dump({"keys": [{"kty": "RSA", "kid": "k1", "alg": "RS256", "n": b64(bytes.fromhex(mod)), "e": "AQAB"}]},
          open(f"{d}/rsa.jwks", "w"))
PY

cd "$HERE/.."
start_server "$HERE/cases/extras.lux" || exit 1
ok "starts"

echo "== gzip =="
check "gzip round trip"                 GET /gzip/basic 200 '"smaller":true,"back":true'
check "gzip.decompress caps the output" GET /gzip/basic 200 '"capped":"gzip.decompress(): the output is over the limit","bad":"gzip.decompress(): invalid data"'

echo "== crypto: encryption =="
check "encrypt/decrypt round trip"      GET /crypto/cipher 200 '"back":"secret ñ","fresh_nonce":true'
check "tampered or other key is null"   GET /crypto/cipher 200 '"tampered":null,"other_key":null'
check "aad binds the token"             GET /crypto/cipher 200 '"aad_ok":"for user 7","aad_wrong":null'
check "a password is not a key"         GET /crypto/bad_key 500 'the key must be 32 bytes from crypto.key()'

echo "== crypto: signatures =="
for case in "RS256 rsa sha256" "PS256 rsa sha256" "ES256 ec sha256" "EdDSA ed -"; do
    set -- $case
    check "$1 sign/verify" GET "/crypto/sign/$1/$2" 200 '"ok":true,"tampered":false'
    # The same signature, checked by openssl on its own.
    sig=$(curl -sS "http://127.0.0.1:$PORT/crypto/sign/$1/$2" | python3 -c "
import json, sys, base64
s = json.load(sys.stdin)['sig']; raw = base64.urlsafe_b64decode(s + '=' * (-len(s) % 4))
if '$1' == 'ES256':   # JWS r||s -> DER, what openssl reads
    from itertools import chain
    def der_int(b):
        b = b.lstrip(b'\0') or b'\0'
        if b[0] & 0x80: b = b'\0' + b
        return b'\x02' + bytes([len(b)]) + b
    body = der_int(raw[:32]) + der_int(raw[32:]); raw = b'\x30' + bytes([len(body)]) + body
open('$TMP/sig.bin', 'wb').write(raw)")
    printf 'hello' > "$TMP/data"
    case $1 in
        EdDSA) out=$(openssl pkeyutl -verify -pubin -inkey "$TMP/ed.pub" -rawin -in "$TMP/data" -sigfile "$TMP/sig.bin" 2>&1) ;;
        PS256) out=$(openssl dgst -sha256 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:digest -verify "$TMP/rsa.pub" -signature "$TMP/sig.bin" "$TMP/data" 2>&1) ;;
        *)     out=$(openssl dgst -$3 -verify "$TMP/$2.pub" -signature "$TMP/sig.bin" "$TMP/data" 2>&1) ;;
    esac
    if echo "$out" | grep -q "Verified OK\|Signature Verified Successfully"; then ok "$1 signature verifies with openssl"
    else fail "$1 signature verifies with openssl" "Verified OK" "$out"; fi
done

echo "== crypto: JWT =="
check "RS256 JWT round trip, aud/iss checked" GET /crypto/jwt/RS256/rsa 200 '"claims":{"sub":"u1","aud":"app","iss":"me"'
check "ES256 JWT, wrong aud / expired are null" GET /crypto/jwt/ES256/ec 200 '"wrong_aud":null,"expired":null'
check "a JWKS picks the key by kid"   GET /crypto/jwk     200 '"claims":{"sub":"jwk"},"unknown_kid":null'
check "alg none, HS256 confusion, alg/key mismatch" GET /crypto/attacks 200 '"none":null,"hs256_with_public_key":null,"rs_alg_on_ec_key":false'

summary
