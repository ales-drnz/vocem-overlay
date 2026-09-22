#!/usr/bin/env python3
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
"""Phase 0b gate: find out what Discord's local RPC actually tells us.

The overlay needs three things per voice participant: identity (name, avatar),
mute/deaf flags, and -- if it is reachable at all -- whether the user is
streaming ("Go Live") or has their camera on. The first two are documented. The
third is an open question: it exists in Discord's gateway protocol, but it is not
documented in the local RPC voice state object, and the Streamkit client id we
authenticate with only holds the rpc, messages.read and rpc.notifications.read
scopes.

This script answers it empirically. It records every raw payload, then reports
the exact schema observed for voice states and every key anywhere in the stream
whose name suggests streaming or video.

Usage:
    scripts/rpc-probe.py                  # run for 120 seconds
    scripts/rpc-probe.py --seconds 300
    scripts/rpc-probe.py --output /tmp/probe.jsonl

Requirements: the Discord desktop client must be running, you must be in a voice
channel, and vocemd must have been authorised once, so that a token exists.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Iterator

try:
    import websocket  # provided by python-websocket-client
except ImportError:
    # Not installed here (it was, on 2026-08-10; it is not on 2026-09-08), and
    # the moment a probe is wanted is a moment somebody is streaming, not a
    # moment to install packages. The handful of the protocol this script uses
    # -- one handshake, masked text frames out, text/ping/close frames in --
    # fits in the standard library below, under the same three names.
    import base64
    import os as _os
    import socket
    import struct
    from urllib.parse import urlparse

    class _Closed(Exception):
        pass

    class _Socket:
        def __init__(self, url: str, origin: str, timeout: float) -> None:
            parts = urlparse(url)
            self.sock = socket.create_connection((parts.hostname, parts.port or 80), timeout)
            key = base64.b64encode(_os.urandom(16)).decode()
            request = (
                f"GET {parts.path or '/'}{'?' + parts.query if parts.query else ''} HTTP/1.1\r\n"
                f"Host: {parts.hostname}:{parts.port or 80}\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                f"Origin: {origin}\r\n\r\n"
            )
            self.sock.sendall(request.encode())
            head = b""
            # Bounded, and on the caller's clock. A peer that accepts and then
            # trickles has to hit something: the daemon's own client learned
            # this twice (entries 72 and 77), and this probe had neither a cap
            # nor a deadline on the handshake.
            deadline = time.monotonic() + max(timeout, 1.0)
            while b"\r\n\r\n" not in head:
                if len(head) > 8192:
                    raise _Closed("handshake: 8 KiB of headers and no blank line")
                if time.monotonic() > deadline:
                    raise _Closed("handshake: no reply within the timeout")
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise _Closed("handshake: connection closed")
                head += chunk
            status = head.split(b"\r\n", 1)[0]
            if b" 101 " not in status:
                raise _Closed(f"handshake refused: {status.decode(errors='replace')}")
            self.buffer = head.split(b"\r\n\r\n", 1)[1]

        def settimeout(self, seconds: float) -> None:
            self.sock.settimeout(seconds)

        def _exactly(self, n: int) -> bytes:
            while len(self.buffer) < n:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise _Closed("connection closed")
                self.buffer += chunk
            out, self.buffer = self.buffer[:n], self.buffer[n:]
            return out

        def _frame(self, opcode: int, payload: bytes) -> None:
            mask = _os.urandom(4)
            header = bytes([0x80 | opcode])
            n = len(payload)
            if n < 126:
                header += bytes([0x80 | n])
            elif n < 65536:
                header += bytes([0x80 | 126]) + struct.pack("!H", n)
            else:
                header += bytes([0x80 | 127]) + struct.pack("!Q", n)
            masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            self.sock.sendall(header + mask + masked)

        def send(self, text: str) -> None:
            self._frame(0x1, text.encode())

        # A frame is taken whole or not at all.
        #
        # This used to consume the two-byte header and then read the payload,
        # so a socket timeout inside the read propagated out of the MIDDLE of a
        # frame -- and run() catches timeouts and continues, so the next recv()
        # read two bytes of payload as a header. From there the stream was
        # desynchronised for the rest of the session and every garbled message
        # landed in `except json.JSONDecodeError: continue`, which is silence;
        # report() then printed "the local RPC does not expose it; drop the
        # feature from scope" on an empty list of hits. This is the instrument
        # phase 0b was settled with, and its own comment already records that
        # it has produced one wrong verdict.
        #
        # The first repair closed the connection on any timeout inside recv(),
        # including one at a frame boundary with nothing read: with run()'s
        # one-second timeout, every session ended after its first quiet second
        # (measured with a loopback peer sending a frame, pausing 2.5 s and
        # sending another: CLOSED at 1.0 s, the second frame never read). Now
        # the bytes stay in the buffer until a whole frame is there, so a
        # timeout -- at a boundary or inside a frame -- consumes nothing and is
        # handed to the caller as the timeout it is.
        def _whole_frame(self):
            """(fin, opcode, payload) once a whole frame is buffered, unmasked."""
            while True:
                b = self.buffer
                if len(b) >= 2:
                    length = b[1] & 0x7F
                    at = 2
                    if length == 126 and len(b) >= 4:
                        length = struct.unpack("!H", b[2:4])[0]
                        at = 4
                    elif length == 127 and len(b) >= 10:
                        length = struct.unpack("!Q", b[2:10])[0]
                        at = 10
                    elif length >= 126:
                        length = None
                    # Discord does not send frames like this; a peer that does
                    # is not one this probe should try to follow.
                    if length is not None and length > 16 * 1024 * 1024:
                        self.sock.close()
                        raise _Closed(f"frame of {length} bytes: past anything this reads")
                    if length is not None:
                        masked = bool(b[1] & 0x80)
                        end = at + (4 if masked else 0) + length
                        if len(b) >= end:
                            mask = b[at:at + 4] if masked else b""
                            payload = b[end - length:end]
                            self.buffer = b[end:]
                            if mask:
                                payload = bytes(x ^ mask[i % 4] for i, x in enumerate(payload))
                            return bool(b[0] & 0x80), b[0] & 0x0F, payload
                chunk = self.sock.recv(65536)  # a timeout here consumes nothing
                if not chunk:
                    raise _Closed("connection closed")
                self.buffer += chunk

        def recv(self) -> str:
            while True:
                fin, opcode, payload = self._whole_frame()
                if opcode == 0x8:
                    raise _Closed("close frame")
                if opcode == 0x9:
                    self._frame(0xA, payload)
                    continue
                if opcode in (0x1, 0x0):
                    if not fin:
                        # Continuations are not reassembled here, and a probe
                        # that returned half a message as a whole one would
                        # report about text it never saw.
                        raise _Closed("fragmented message: this probe does not reassemble")
                    return payload.decode(errors="replace")
                # binary or pong: nothing this probe reads

    class websocket:  # type: ignore[no-redef]  # the three names the script uses
        WebSocketTimeoutException = socket.timeout
        WebSocketConnectionClosedException = _Closed

        @staticmethod
        def create_connection(url: str, origin: str, timeout: float) -> _Socket:
            return _Socket(url, origin, timeout)

# Streamkit's public client id, the same one vocemd authorises with, so the token
# it stored is valid here.
CLIENT_ID = "207646673902501888"
RPC_URL = f"ws://127.0.0.1:6463/?v=1&client_id={CLIENT_ID}"
RPC_ORIGIN = "http://localhost:3000"

# Where vocemd stores the token it was granted.
TOKEN_PATH = (
    Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    / "vocem"
    / "token"
)

# Key names that would indicate the data we are hunting for.
INTERESTING = ("stream", "video", "live", "screenshare", "activity")


def load_access_token() -> str:
    if not TOKEN_PATH.is_file():
        sys.exit(
            f"error: {TOKEN_PATH} not found.\n"
            "Start vocemd once with Discord open and accept the authorisation "
            "prompt, then try again."
        )
    token = TOKEN_PATH.read_text(encoding="utf-8").strip()
    if not token:
        sys.exit(f"error: {TOKEN_PATH} is empty; start vocemd to authorise again.")
    return token


def walk(obj: Any, path: str = "") -> Iterator[tuple[str, Any]]:
    """Yield (dotted path, value) for every leaf and every dict key."""
    if isinstance(obj, dict):
        for key, value in obj.items():
            child = f"{path}.{key}" if path else key
            yield child, value
            yield from walk(value, child)
    elif isinstance(obj, list):
        for item in obj:
            yield from walk(item, f"{path}[]")


# Beside the session's own runtime directory rather than in /tmp, which is shared
# with every other user on the machine.
def default_output() -> Path:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    root = Path(runtime) if runtime else Path.home()
    return root / f"vocem-rpc-probe-{os.getpid()}.jsonl"


class Probe:
    def __init__(self, output: Path) -> None:
        # What this writes is every payload received, NOTIFICATION_CREATE bodies
        # among them: the full text of the user's direct messages. Created for
        # this user only, refusing to follow a symlink and refusing to reuse a
        # name somebody else may have placed there first.
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        self.output = os.fdopen(fd, "w", encoding="utf-8")
        self.token = load_access_token()
        self.ws = websocket.create_connection(RPC_URL, origin=RPC_ORIGIN, timeout=5)
        self.channel_id: str | None = None
        self.voice_state_keys: set[str] = set()
        self.hits: dict[str, set[str]] = {}
        self.events_seen: dict[str, int] = {}

    # -- protocol ---------------------------------------------------------

    def send(self, payload: dict) -> None:
        self.ws.send(json.dumps(payload))

    def authenticate(self) -> None:
        self.send({"cmd": "AUTHENTICATE", "args": {"access_token": self.token}, "nonce": "auth"})

    def subscribe(self, event: str, args: dict | None = None) -> None:
        self.send({"cmd": "SUBSCRIBE", "args": args or {}, "evt": event, "nonce": event})

    def subscribe_voice(self, channel_id: str) -> None:
        for event in (
            "VOICE_STATE_CREATE",
            "VOICE_STATE_UPDATE",
            "VOICE_STATE_DELETE",
            "SPEAKING_START",
            "SPEAKING_STOP",
        ):
            self.subscribe(event, {"channel_id": channel_id})
        print(f"  subscribed to voice events on channel {channel_id}")

    # -- analysis ---------------------------------------------------------

    def inspect(self, message: dict) -> None:
        name = message.get("evt") or message.get("cmd") or "?"
        self.events_seen[name] = self.events_seen.get(name, 0) + 1

        for path, value in walk(message):
            # The channel snapshot carries the channel's recent text messages,
            # and a message's embed of a tweet or a Twitch link has a `video`
            # key of its own: that is content somebody posted, not a
            # participant's state, and on 2026-09-08 it made this report say
            # FEASIBLE about a channel whose voice states said nothing.
            if path.startswith("data.messages"):
                continue
            leaf = path.rsplit(".", 1)[-1].removesuffix("[]").lower()
            if any(token in leaf for token in INTERESTING):
                self.hits.setdefault(path, set()).add(json.dumps(value)[:80])

        # Record the shape of every voice state we come across, wherever it is.
        for path, value in walk(message):
            if path.endswith("voice_state") and isinstance(value, dict):
                self.voice_state_keys.update(value.keys())

    def handle(self, message: dict) -> None:
        self.output.write(json.dumps(message) + "\n")
        self.output.flush()
        self.inspect(message)

        command = message.get("cmd")
        event = message.get("evt")
        data = message.get("data") or {}

        if event == "READY":
            print("  RPC ready, authenticating")
            self.authenticate()
        elif command == "AUTHENTICATE":
            if event == "ERROR":
                sys.exit(f"error: authentication rejected: {data}")
            user = data.get("user", {})
            print(f"  authenticated as {user.get('username', '?')}")
            self.subscribe("VOICE_CHANNEL_SELECT")
            self.send({"cmd": "GET_SELECTED_VOICE_CHANNEL", "args": {}, "nonce": "channel"})
        elif command == "GET_SELECTED_VOICE_CHANNEL":
            if not data:
                print("  not currently in a voice channel -- join one now")
                return
            self.channel_id = data.get("id")
            members = data.get("voice_states", [])
            print(f"  in '{data.get('name')}' with {len(members)} participant(s)")
            if self.channel_id:
                self.subscribe_voice(self.channel_id)
        elif event == "VOICE_CHANNEL_SELECT":
            new_channel = data.get("channel_id")
            if new_channel and new_channel != self.channel_id:
                self.channel_id = new_channel
                self.subscribe_voice(new_channel)

    # -- main loop --------------------------------------------------------

    def run(self, seconds: int) -> None:
        deadline = time.monotonic() + seconds
        self.ws.settimeout(1.0)
        while time.monotonic() < deadline:
            try:
                raw = self.ws.recv()
            except websocket.WebSocketTimeoutException:
                continue
            except (websocket.WebSocketConnectionClosedException, OSError):
                print("  connection closed by Discord")
                break
            if not raw:
                continue
            try:
                self.handle(json.loads(raw))
            except json.JSONDecodeError:
                continue

    def report(self) -> None:
        print("\n" + "=" * 72)
        print("EVENTS RECEIVED")
        for name, count in sorted(self.events_seen.items(), key=lambda kv: -kv[1]):
            print(f"  {count:5d}  {name}")

        print("\nVOICE STATE SCHEMA (union of every voice_state object seen)")
        if self.voice_state_keys:
            for key in sorted(self.voice_state_keys):
                print(f"  {key}")
        else:
            print("  none observed")

        print("\nKEYS SUGGESTING STREAM / VIDEO / LIVE")
        if self.hits:
            for path, values in sorted(self.hits.items()):
                print(f"  {path} = {', '.join(sorted(values))}")
            print("\n  -> the live-status feature is FEASIBLE, using the paths above")
        else:
            print("  none found anywhere in the payloads")
            print("\n  -> the local RPC does not expose it; drop the feature from scope")
        print("=" * 72)
        print(f"\nRaw payloads: {self.output.name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=120, help="how long to listen")
    parser.add_argument(
        "--output",
        type=Path,
        default=default_output(),
        help="where to write raw payloads (message text included)",
    )
    args = parser.parse_args()

    print(f"Connecting to Discord RPC (listening for {args.seconds}s)")
    probe = Probe(args.output)
    try:
        probe.run(args.seconds)
    except KeyboardInterrupt:
        print("\n  interrupted")
    finally:
        probe.report()


if __name__ == "__main__":
    main()
