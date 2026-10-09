#!/usr/bin/env bash
# `mailparse` native module on a nasty multipart message. Usage: tests/run_mailparse.sh [lux-binary]
set -u
cd "$(dirname "$0")/.."
LUX=${1:-build/lux}; WEB=${MAILPARSE_TEST_WEB:-18092}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/app.lux" <<'LUX'
import mailparse
app:
    name "mp"

get endpoint("/p"):
    string raw = "From: =?UTF-8?B?QW5hIEzDs3Bleg==?= <Ana@X.com>\r\nTo: yo@x.com\r\nSubject: =?UTF-8?Q?Presupuesto_=C3=B1?=\r\n =?UTF-8?Q?_final?=\r\nDate: Wed, 07 Oct 2026 10:00:00 +0200\r\nMessage-ID: <a@x>\r\nContent-Type: multipart/mixed; boundary=\"OUT\"\r\n\r\n--OUT\r\nContent-Type: multipart/alternative; boundary=\"IN\"\r\n\r\n--IN\r\nContent-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nHola ma=F1ana=\r\n !\r\n--IN\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>hola</p>\r\n--IN--\r\n--OUT\r\nContent-Type: application/pdf; name=\"x.pdf\"\r\nContent-Disposition: attachment; filename*=UTF-8''caf%C3%A9.pdf\r\nContent-Transfer-Encoding: base64\r\nContent-ID: <c1@x>\r\n\r\nSGVsbG8=\r\n--OUT--\r\n"
    return { "m": mailparse.parse(raw), "att": mailparse.attachment(raw, 0) }

get endpoint("/inline"):
    string raw = "From: a@x.com\r\nSubject: logo\r\nContent-Type: multipart/related; boundary=R\r\n\r\n--R\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>Hola</p><img src=\"cid:logo1\"><img src=\"cid:logo1\">\r\n--R\r\nContent-Type: image/png; name=logo.png\r\nContent-ID: <logo1>\r\nContent-Disposition: inline; filename=logo.png\r\nContent-Transfer-Encoding: base64\r\n\r\niVBORw0KGgo=\r\n--R--\r\n"
    return { "m": mailparse.parse(raw), "bytes": mailparse.attachment(raw, 0) }
LUX
"$LUX" "$T/app.lux" --port $WEB >"$T/srv.log" 2>&1 & sleep 1.5
R=$(curl -s -m 10 localhost:$WEB/p); echo "$R"
fail=0
check() { if grep -qF -- "$1" <<<"$R"; then echo "ok   $2"; else echo "FAIL $2: wanted [$1]"; fail=1; fi; }
check '"subject":"Presupuesto ñ final"' "RFC 2047 subject, folded + joined words"
check '"from":"Ana López \u003cAna@X.com\u003e"' "encoded display name"
check '"from_email":"ana@x.com"' "from_email lowercased"
check '"date":1791360000' "Date with timezone -> unix"
check '"text":"Hola mañana !' "quoted-printable + latin1 + soft break"
check '"html":"\u003cp\u003ehola\u003c/p\u003e"' "html alternative"
check '"name":"café.pdf"' "RFC 2231 filename"
check '"cid":"c1@x"' "content-id"
check '"att":"Hello"' "attachment bytes"
I=$(curl -s -m 10 localhost:$WEB/inline); R=$I
check 'data:image/png;base64,iVBORw0KGgo=' "imagen cid: embebida como data: URI en html"
check '"html_cid":"\u003cp\u003eHola\u003c/p\u003e\u003cimg src=\"cid:logo1\"\u003e' "html_cid conserva la referencia cid: original"
check '"cid":"logo1"' "el adjunto inline conserva su cid"
if [ "$(grep -o 'data:image/png;base64,iVBORw0KGgo=' <<<"$I" | wc -l)" = 2 ]; then echo "ok   todas las referencias a la misma imagen se sustituyen"; else echo "FAIL no se sustituyeron todas"; fail=1; fi
exit $fail
