#!/usr/bin/env python3
"""Tiny local echo server for tests/run_http.sh -- no internet access needed
to verify the `http` native module (NATIVE-MODULES.md): reflects method,
headers and body back as JSON, and serves a fixed status code on demand.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Echo(BaseHTTPRequestHandler):
    def _reply(self, status=200, obj=None):
        body = json.dumps(obj if obj is not None else {}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _reflect(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        try:
            parsed_body = json.loads(raw) if raw else None
        except ValueError:
            parsed_body = raw
        self._reply(200, {
            "method": self.command,
            "path": self.path,
            "headers": {k.lower(): v for k, v in self.headers.items()},
            "body": parsed_body,
        })

    def do_GET(self):
        if self.path.startswith("/status/"):
            code = int(self.path.rsplit("/", 1)[-1])
            self._reply(code, {"forced_status": code})
            return
        self._reflect()

    def do_POST(self):   self._reflect()
    def do_PUT(self):    self._reflect()
    def do_PATCH(self):  self._reflect()
    def do_DELETE(self): self._reflect()

    def log_message(self, *a):  # keep the test output quiet
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    HTTPServer(("127.0.0.1", port), Echo).serve_forever()
