#!/usr/bin/env python3
"""Orderly shutdown (App::run drain): idle keep-alive connections must not hold it up.

    tests/shutdown.py <lux-binary> [port]

1. 4 keep-alive connections that already got their answer (like a browser's) and stay open:
   SIGTERM must end the process in well under the 30 s grace period.
2. A request in flight when SIGTERM arrives is still answered (Connection: close), then exit.
"""
import os, signal, socket, subprocess, sys, tempfile, time

lux = os.path.abspath(sys.argv[1])
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8861
APP = 'get endpoint("/ping"):\n    return { "ok": true }\n\nget endpoint("/lento"):\n    await sleep(1500)\n    return { "lento": true }\n'


def arrancar(tmp):
    open(os.path.join(tmp, "app.lux"), "w").write(APP)
    p = subprocess.Popen([lux, "app.lux", "--port", str(port)], cwd=tmp, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return p
        except OSError:
            time.sleep(0.1)
    p.kill()
    sys.exit("el servidor no arrancó")


fallos = 0


def ok(nombre, cond, extra=""):
    global fallos
    print(("  ok    " if cond else "  FAIL  ") + nombre + (f" ({extra})" if extra else ""))
    fallos += 0 if cond else 1


with tempfile.TemporaryDirectory() as tmp:
    p = arrancar(tmp)
    socks = []
    for _ in range(4):
        s = socket.create_connection(("127.0.0.1", port))
        s.sendall(b"GET /ping HTTP/1.1\r\nHost: x\r\n\r\n")
        s.recv(4096)
        socks.append(s)
    t = time.time()
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
    ok("keep-alive inactivas no retrasan el apagado", time.time() - t < 3, f"{time.time() - t:.2f} s")

with tempfile.TemporaryDirectory() as tmp:
    p = arrancar(tmp)
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"GET /lento HTTP/1.1\r\nHost: x\r\n\r\n")
    time.sleep(0.3)
    t = time.time()
    p.send_signal(signal.SIGTERM)
    s.settimeout(10)
    data = b""
    try:
        while True:
            c = s.recv(4096)
            if not c:
                break
            data += c
    except OSError:
        pass
    try:
        p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
    ok("la petición en curso se responde", b'"lento":true' in data)
    ok("con Connection: close", b"onnection: close" in data)
    ok("y sale enseguida después", time.time() - t < 5, f"{time.time() - t:.2f} s")

print("todo ok" if not fallos else f"{fallos} fallo(s)")
sys.exit(1 if fallos else 0)
