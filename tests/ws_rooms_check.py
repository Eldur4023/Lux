#!/usr/bin/env python3
# Two WebSocket clients in one room (cases/modules.lux, /room/:id):
# what A sends reaches B as JSON and not A, and count follows joins/leaves.
#   ws_rooms_check.py <port>   -> prints "ok" or what went wrong
import base64, json, os, socket, struct, sys

PORT = int(sys.argv[1])

def connect(path):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: http://localhost\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    assert b" 101 " in head.split(b"\r\n")[0], head
    return s

def send(s, text):
    data, mask = text.encode(), os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def recv(s):
    b0, b1 = s.recv(2)
    n = b1 & 0x7F
    if n == 126: n = struct.unpack(">H", s.recv(2))[0]
    data = b""
    while len(data) < n: data += s.recv(n - len(data))
    return data.decode()

def quiet(s):
    s.settimeout(0.3)
    try: return recv(s)
    except socket.timeout: return None
    finally: s.settimeout(5)

a, b = connect("/room/7"), connect("/room/7")
assert recv(a) == "joined" and recv(b) == "joined"
send(a, "hi")
got = recv(b)
assert json.loads(got) == {"from": "hi"}, got
assert quiet(a) is None, "the sender got its own message back"
send(b, "count"); assert recv(b) == "2"
a.close()
import time; time.sleep(0.3)
send(b, "count"); n = recv(b)
assert n == "1", "count after one client left: " + n
print("ok")
