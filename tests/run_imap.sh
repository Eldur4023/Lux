#!/usr/bin/env bash
# `imap` native module against tests/imap_fake_server.py (no real mail server needed).
# Usage: tests/run_imap.sh [path-to-lux-binary]
set -u
cd "$(dirname "$0")/.."
LUX=${1:-build/lux}
PORT=${IMAP_TEST_PORT:-14143}; WEB=${IMAP_TEST_WEB:-18091}
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
python3 tests/imap_fake_server.py $PORT "$T/wire.log" nomove & sleep 0.5
cat > "$T/app.lux" <<LUX
import imap
app:
    name "imap-test"

get endpoint("/all"):
    Json c = { "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "pw" }
    Json bad = { "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "wrong" }
    return {
        "folders": await imap.list_folders(c),
        "status": await imap.status(c, "INBOX"),
        "uids": await imap.uids(c, "INBOX", 6),
        "head": await imap.fetch(c, "INBOX", 5, "HEADER"),
        "full": await imap.fetch(c, "INBOX", 9),
        "flags": await imap.set_flags(c, "INBOX", 9, "add", ["\\\\Seen"]),
        "move": await imap.move(c, "INBOX", 9, "Trash"),
        "append": await imap.append(c, "[Gmail]/Sent Mail", "Subject: x\r\n\r\nhi\r\n")
    }

get endpoint("/folders"):
    Json c = { "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "pw" }
    await imap.create_folder(c, "Año")
    List<Json> made = await imap.list_folders(c)
    await imap.rename_folder(c, "Año", "Año 2026 & más")
    List<Json> renamed = await imap.list_folders(c)
    await imap.delete_folder(c, "Año 2026 & más")
    List<Json> gone = await imap.list_folders(c)
    return { "made": made, "renamed": renamed, "gone": gone }

get endpoint("/badname"):
    return await imap.create_folder({ "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "pw" }, "a\r\nx LOGOUT")

get endpoint("/login"):
    return await imap.uids({ "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "wrong" }, "INBOX", 1)

get endpoint("/inject"):
    return await imap.status({ "host": "127.0.0.1", "port": $PORT, "tls": "none", "user": "me", "password": "pw" }, "A\r\nx LOGOUT")
LUX
"$LUX" "$T/app.lux" --port $WEB >"$T/srv.log" 2>&1 & sleep 1.5
fail=0
check() { if grep -qF -- "$2" <<<"$1"; then echo "ok   $3"; else echo "FAIL $3: wanted [$2]"; fail=1; fi; }
R=$(curl -s -m 20 localhost:$WEB/all); echo "$R" | head -c 1500; echo
check "$R" '"name":"[Gmail]/Sent Mail","attrs":["\\HasNoChildren","\\Sent"]' "list_folders parses name + special-use"
check "$R" '"uidnext":10' "status"
check "$R" '"uids":[{"uid":9,"flags":[]}]' "uids filters below since"
check "$R" 'Subject: one' "fetch header section"
check "$R" 'body two' "fetch whole message"
W=$(cat "$T/wire.log")
check "$W" 'UID STORE 9 +FLAGS.SILENT (\Seen)' "set_flags command"
check "$W" 'UID EXPUNGE 9' "move falls back to copy+delete+expunge"
check "$W" 'APPENDED: Subject: x||hi|' "append body"
check "$(curl -s -m 10 localhost:$WEB/login)" 'login failed' "auth failure surfaces"
curl -s -m 10 localhost:$WEB/inject >/dev/null
F=$(curl -s -m 20 localhost:$WEB/folders)
check "$F" '"made":[' "carpetas: crear y listar"
python3 - "$F" <<'PY' && echo "ok   los nombres con tildes y & se decodifican al listar" || { echo "FAIL nombres UTF-8"; fail=1; }
import sys, json
d = json.loads(sys.argv[1])
names = lambda k: [f["name"] for f in d[k]]
assert "Año" in names("made"), names("made")
assert "Año 2026 & más" in names("renamed") and "Año" not in names("renamed"), names("renamed")
assert "Año 2026 & más" not in names("gone"), names("gone")
PY
W2=$(cat "$T/wire.log")
check "$W2" 'CREATE "A&APE-o"' "UTF-7 modificado en CREATE (Año -> A&APE-o)"
check "$W2" 'RENAME "A&APE-o" "A&APE-o 2026 &- m&AOE-s"' "UTF-7 modificado en RENAME (& literal = &-)"
check "$W2" 'DELETE "A&APE-o 2026 &- m&AOE-s"' "DELETE con el nombre codificado"
curl -s -m 10 localhost:$WEB/badname >/dev/null
if grep -q '^x LOGOUT' "$T/wire.log"; then echo "FAIL un nombre de carpeta inyectó un comando"; fail=1; else echo "ok   un nombre con CR/LF se rechaza"; fi
if grep -q '^x LOGOUT' "$T/wire.log"; then echo "FAIL folder name injected a command"; fail=1; else echo "ok   CRLF in folder name rejected"; fi
exit $fail
