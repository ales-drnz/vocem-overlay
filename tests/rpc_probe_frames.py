# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# scripts/rpc-probe.py's own WebSocket client against a loopback peer that
# pauses: once between two frames, once in the middle of one (DESIGN 206).
#
# The probe is what phase 0b was settled with, and a client that loses the
# stream reports "the local RPC does not expose it" on an empty list. Its
# standard-library fallback is the client that runs here (the `websocket`
# module is not installed), so that is the one exercised: the fallback block is
# taken out of the script and run as it stands. run() reads with a one-second
# timeout, so each pause is longer than that; both frames must arrive, whole
# and in order, and nothing may close the connection before the peer does.

import base64
import hashlib
import socket
import sys
import threading
import time

source = open(sys.argv[1]).read()
start = source.index("try:\n    import websocket")
end = source.index("# Streamkit")
namespace = {"time": time, "sys": sys}
# The fallback, whatever is installed: the import is made to fail.
exec(source[start:end].replace("import websocket", "import __no_such_module__", 1), namespace)
ws = namespace["websocket"]

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", 0))
server.listen(1)
port = server.getsockname()[1]


def serve():
    conn, _ = server.accept()
    data = b""
    while b"\r\n\r\n" not in data:
        data += conn.recv(4096)
    key = [l.split(b": ")[1] for l in data.split(b"\r\n")
           if l.lower().startswith(b"sec-websocket-key")][0]
    accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    conn.sendall(b"HTTP/1.1 101 Switching\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")

    def frame(text):
        payload = text.encode()
        return bytes([0x81, len(payload)]) + payload

    conn.sendall(frame('{"first":1}'))
    time.sleep(1.6)                      # a quiet second and more, at a frame boundary
    second = frame('{"second":2}')
    conn.sendall(second[:5])
    time.sleep(1.6)                      # and in the middle of a frame
    conn.sendall(second[5:])
    time.sleep(0.5)
    conn.close()


threading.Thread(target=serve, daemon=True).start()
client = ws.create_connection(f"ws://127.0.0.1:{port}/?v=1", origin="x", timeout=5)
client.settimeout(1.0)
received = []
closed_by = None
began = time.monotonic()
while time.monotonic() - began < 8:
    try:
        received.append(client.recv())
    except ws.WebSocketTimeoutException:
        continue
    except (ws.WebSocketConnectionClosedException, OSError) as error:
        closed_by = str(error)
        break
print(f"     received {received}; closed after {time.monotonic() - began:.1f} s: {closed_by}")
failures = 0
for ok, what in [
    (received == ['{"first":1}', '{"second":2}'],
     "both frames arrive, whole and in order, across a pause at a boundary and one inside a frame"),
    (closed_by == "connection closed", "and the connection ends when the peer ends it, not before"),
]:
    print(("ok   " if ok else "FAIL ") + what)
    failures += 0 if ok else 1
sys.exit(1 if failures else 0)
