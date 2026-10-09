#!/usr/bin/env python3
"""Tiny fake IMAP server for tests/run_imap.sh -- just enough of RFC 3501 (no TLS) to
exercise the `imap` native module without a real mail server. One folder, INBOX with
two messages; every command it receives is appended to the file given as argv[2] so the
test can check what actually went over the wire.
"""
import re
import socketserver
import sys

# uid -> [raw message, flags string]; shared by every connection and mutable (STORE, X-TEST)
MSGS = {5: [b"Subject: one\r\nFrom: a@x.com\r\n\r\nbody one\r\n", r"\Seen"],
        9: [b"Subject: dos\r\nFrom: b@x.com\r\n\r\nbody two\r\n", ""]}
TRASH, ARCHIVE, JUNK, CLIENTES = {}, {}, {}, {}
FOLDERS = {"INBOX": None, "[Gmail]/Sent Mail": None, "Drafts": None, "Trash": TRASH, "Archive": ARCHIVE, "Junk": JUNK, "Clientes": CLIENTES}   # None = filled below
DRAFTS = {}   # the Drafts folder: its own store, filled by APPEND and emptied by STORE \\Deleted + UID EXPUNGE


FOLDERS.update({"INBOX": MSGS, "[Gmail]/Sent Mail": MSGS, "Drafts": DRAFTS})
SPECIAL = {"[Gmail]/Sent Mail": "\\Sent", "Drafts": "\\Drafts", "Trash": "\\Trash", "Archive": "\\Archive", "Junk": "\\Junk"}   # SPECIAL-USE flags
FAIL = set()   # commands that answer NO: X-TEST FAIL MOVE / X-TEST OK MOVE
LAST = {}   # per folder: highest uid ever given (IMAP never reuses a uid within a UIDVALIDITY)


def new_uid(store):
    LAST[id(store)] = max(LAST.get(id(store), 0), max(store, default=0)) + 1
    return LAST[id(store)]


class H(socketserver.StreamRequestHandler):
    def send(self, s):
        self.wfile.write(s if isinstance(s, bytes) else s.encode())

    def handle(self):
        log = open(sys.argv[2], "a")
        sel = "INBOX"                                  # folder chosen by the last SELECT on this connection
        store = lambda: FOLDERS.get(sel, MSGS)
        self.send("* OK fake imap ready\r\n")
        while line := self.rfile.readline():
            line = line.decode().rstrip("\r\n")
            log.write(line + "\n"); log.flush()
            tag, _, cmd = line.partition(" ")
            up = cmd.upper()
            if up == "CAPABILITY":
                self.send("* CAPABILITY IMAP4rev1\r\n")
            elif up.startswith("LOGIN") and "wrong" in cmd:
                self.send(f"{tag} NO [AUTHENTICATIONFAILED] bad login\r\n"); continue
            elif up.startswith("LIST"):
                self.send("".join(f'* LIST (\\HasNoChildren{" " + SPECIAL[n] if n in SPECIAL else ""}) "/" "{n}"\r\n' for n in FOLDERS))
            elif up.startswith("CREATE"):
                FOLDERS.setdefault(cmd.split('"')[1], {})
            elif up.startswith("RENAME"):
                a, b = cmd.split('"')[1], cmd.split('"')[3]
                if a not in FOLDERS or b in FOLDERS: self.send(f"{tag} NO [CANNOT] rename failed\r\n"); continue
                FOLDERS[b] = FOLDERS.pop(a)
            elif up.startswith("DELETE"):
                FOLDERS.pop(cmd.split('"')[1], None)
            elif up.startswith("STATUS"):
                self.send('* STATUS "INBOX" (MESSAGES 2 UIDNEXT 10 UIDVALIDITY 77 UNSEEN 1)\r\n')
            elif up.startswith("SELECT"):
                sel = cmd.split(" ", 1)[1].strip().strip('"')
                self.send("* 2 EXISTS\r\n* OK [UIDVALIDITY 77] ok\r\n")
            elif up.startswith("X-TEST"):   # simulate another client: X-TEST DELETE 9 | X-TEST FLAGS 5 \Seen
                _, op, uid, *rest = cmd.split(" ")
                if op.upper() == "DELETE": MSGS.pop(int(uid), None)
                elif op.upper() == "FAIL": FAIL.add(uid)
                elif op.upper() == "OK": FAIL.discard(uid)
                elif op.upper() == "DELFOLDER": FOLDERS.pop(uid, None)   # X-TEST DELFOLDER <name>: another client deleted it
                elif op.upper() == "ADD": MSGS[int(uid)] = [__import__("base64").b64decode(rest[0]), ""]   # X-TEST ADD <uid> <base64 raw>
                else: MSGS[int(uid)][1] = " ".join(rest)
            elif up.startswith("UID STORE"):
                M = store()
                _, _, uid, mode, *fl = cmd.split(" ")
                words = set(" ".join(fl).strip("()").split())
                cur = set(M[int(uid)][1].split())
                M[int(uid)][1] = " ".join(sorted(cur | words if mode.startswith("+") else cur - words if mode.startswith("-") else words))
            elif up.startswith("UID FETCH") and "(FLAGS)" in up:
                M = store()
                since = int(re.search(r"FETCH (\d+):", up).group(1))
                for n, uid in enumerate(sorted(M), 1):
                    if uid >= since or uid == max(M):
                        self.send(f"* {n} FETCH (UID {uid} FLAGS ({M[uid][1]}))\r\n")
            elif up.startswith("UID FETCH"):
                uid = int(re.search(r"FETCH (\d+)", up).group(1))
                body = store()[uid][0]
                if "HEADER" in up: body = body.split(b"\r\n\r\n")[0] + b"\r\n\r\n"
                self.send(f"* 1 FETCH (UID {uid} BODY[] {{{len(body)}}}\r\n".encode() + body + b")\r\n")
            elif up.startswith("APPEND"):
                n = int(re.search(r"\{(\d+)\+?\}", cmd).group(1))
                box = cmd.split(" ")[1].strip('"')
                self.send("+ go\r\n")
                data = self.rfile.read(n)
                log.write("APPENDED: " + data.decode().replace("\r\n", "|") + "\n"); log.flush()
                self.rfile.readline()
                if box == "Drafts":
                    DRAFTS[new_uid(DRAFTS)] = [data, ""]
            elif up.startswith("UID SEARCH HEADER MESSAGE-ID"):
                want = cmd.split('"')[1].lower().encode()
                hits = [str(u) for u, (raw, _) in sorted(store().items()) if b"message-id: " + want in raw.lower()]
                self.send("* SEARCH " + " ".join(hits) + "\r\n")
            elif up.startswith("UID EXPUNGE"):
                M = store(); uid = int(cmd.split(" ")[2])
                if uid in M and r"\Deleted" in M[uid][1]: del M[uid]
            elif up.startswith("UID MOVE") and "nomove" in sys.argv:
                self.send(f"{tag} BAD unknown command\r\n"); continue
            elif up.startswith("UID COPY") and "COPY" in FAIL:
                self.send(f"{tag} NO [CANNOT] copy refused\r\n"); continue
            elif up.startswith("UID MOVE") and "MOVE" in FAIL:
                self.send(f"{tag} NO [CANNOT] move refused\r\n"); continue
            elif up.startswith("UID MOVE"):
                M = store(); uid = int(cmd.split(" ")[2]); dest = FOLDERS[cmd.split('"')[1]]
                if uid in M and dest is not M:
                    dest[new_uid(dest)] = M.pop(uid)
            elif up.startswith("LOGOUT"):
                self.send(f"* BYE\r\n{tag} OK bye\r\n"); return
            self.send(f"{tag} OK done\r\n")


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
