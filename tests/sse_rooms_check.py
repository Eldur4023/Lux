#!/usr/bin/env python3
# Two SSE streams in one room (cases/modules.lux, /news/:id): a broadcast
# reaches both as a data: event, and one that disconnects is not counted.
#   sse_rooms_check.py <port>   -> prints "ok" or what went wrong
import json, socket, sys, time, urllib.request

PORT = int(sys.argv[1])

class Stream:
    def __init__(self, path):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.sock.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n".encode())
        self.buf = b""
    def recv(self, n): return self.sock.recv(n)
    def close(self): self.sock.close()

def event(s):   # the next "data: " line's text
    while True:
        i = s.buf.find(b"data: ")
        j = s.buf.find(b"\n", i) if i >= 0 else -1
        if j >= 0:
            line, s.buf = s.buf[i + 6:j].decode(), s.buf[j + 1:]
            return line
        chunk = s.recv(4096)
        assert chunk, "the stream closed"
        s.buf += chunk

def publish():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/news/7/publish", timeout=5) as r:
        return json.loads(r.read())

a, b = Stream("/news/7"), Stream("/news/7")
assert event(a) == "joined" and event(b) == "joined"
r = publish()
assert r == {"reached": 2, "count": 2}, r
for s in (a, b):
    got = event(s)
    assert json.loads(got) == {"headline": "hello 7"}, got
a.close()
time.sleep(0.3)
r = publish()
assert r == {"reached": 1, "count": 1}, r
assert json.loads(event(b)) == {"headline": "hello 7"}
print("ok")
