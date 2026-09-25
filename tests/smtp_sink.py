#!/usr/bin/env python3
"""A tiny SMTP server for tests/run_http.sh (mail module): accepts every
message and writes the envelope and data of the last one to <outfile> as
JSON. Plain SMTP only -- enough to check what Lux sends."""
import json, socketserver, sys

OUT = sys.argv[2]

class Session(socketserver.StreamRequestHandler):
    def handle(self):
        say = lambda line: self.wfile.write((line + "\r\n").encode())
        say("220 sink ready")
        env = {"from": None, "rcpt": []}
        while True:
            line = self.rfile.readline().decode(errors="replace").rstrip("\r\n")
            if not line: return
            cmd = line[:4].upper()
            if cmd in ("EHLO", "HELO"): say("250 sink")
            elif cmd == "MAIL": env["from"] = line[10:]; say("250 ok")
            elif cmd == "RCPT": env["rcpt"].append(line[8:]); say("250 ok")
            elif cmd == "DATA":
                say("354 go ahead")
                data = []
                while (l := self.rfile.readline().decode(errors="replace")) not in (".\r\n", ""):
                    data.append(l)
                json.dump({**env, "data": "".join(data)}, open(OUT, "w"))
                say("250 queued")
            elif cmd == "QUIT": say("221 bye"); return
            else: say("250 ok")

socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer(("127.0.0.1", int(sys.argv[1])), Session).serve_forever()
