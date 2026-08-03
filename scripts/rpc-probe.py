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
    sys.exit("error: python-websocket-client is not installed")

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
