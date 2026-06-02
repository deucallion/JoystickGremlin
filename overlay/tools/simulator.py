"""Replay a scripted Mumble session to the overlay over UDP — no Mumble needed.

Useful for developing/positioning the overlay and for demos. Run the overlay in
one terminal, then:

    python -m tools.simulator              # from the overlay/ directory
    python tools/simulator.py --port 27812

It connects users, then loops a small "conversation" of talk events.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import time

DEFAULT_HOST = os.environ.get("MUMBLE_VOICE_OVERLAY_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("MUMBLE_VOICE_OVERLAY_PORT", "27812"))

USERS = [
    {
        "id": 1,
        "name": "Reaver",
        "channel": "Alpha Squad",
        "channelId": 10,
        "comment": "Gunner. Comms on tac-1.",
        "hash": "a1b2c3",
        "locallyMuted": False,
        "self": False,
    },
    {
        "id": 2,
        "name": "Nova",
        "channel": "Alpha Squad",
        "channelId": 10,
        "comment": "Pilot — Constellation Andromeda",
        "hash": "d4e5f6",
        "locallyMuted": False,
        "self": False,
    },
    {
        "id": 3,
        "name": "Doc",
        "channel": "Alpha Squad",
        "channelId": 10,
        "comment": "",
        "hash": "99aa88",
        "locallyMuted": True,
        "self": False,
    },
    {
        "id": 42,
        "name": "You",
        "channel": "Alpha Squad",
        "channelId": 10,
        "comment": "",
        "hash": "self00",
        "locallyMuted": False,
        "self": True,
    },
]

# (delay before sending, message)
SCRIPT = [
    (0.0, {"t": "meta", "event": "hello", "protocol": 1, "plugin": "simulator"}),
    (0.2, {"t": "server", "event": "synchronized"}),
    (0.0, {"t": "self", "id": 42, "muted": False, "deafened": False, "mode": "push-to-talk"}),
]


def _conversation() -> list[tuple[float, dict]]:
    """A looping back-and-forth of talk events across the squad."""
    return [
        (0.4, {"t": "talk", "id": 1, "state": "talking"}),
        (1.6, {"t": "talk", "id": 1, "state": "passive"}),
        (0.3, {"t": "talk", "id": 2, "state": "talking"}),
        (0.5, {"t": "talk", "id": 1, "state": "talking"}),   # overlap
        (1.2, {"t": "talk", "id": 2, "state": "passive"}),
        (0.6, {"t": "talk", "id": 42, "state": "talking"}),  # you, PTT
        (1.0, {"t": "talk", "id": 1, "state": "passive"}),
        (0.4, {"t": "talk", "id": 42, "state": "passive"}),
        (0.5, {"t": "talk", "id": 3, "state": "shouting"}),  # cross-channel shout
        (1.4, {"t": "talk", "id": 3, "state": "passive"}),
        (0.6, {"t": "talk", "id": 2, "state": "whispering"}),
        (1.3, {"t": "talk", "id": 2, "state": "passive"}),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--once", action="store_true", help="run the conversation once and exit")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)

    def send(msg: dict) -> None:
        sock.sendto((json.dumps(msg) + "\n").encode("utf-8"), dest)

    print(f"Simulating Mumble voice activity -> {args.host}:{args.port} (Ctrl+C to stop)")
    for user in USERS:
        send({"t": "user", **user})
    for delay, msg in SCRIPT:
        time.sleep(delay)
        send(msg)

    try:
        while True:
            for delay, msg in _conversation():
                time.sleep(delay)
                send(msg)
            if args.once:
                break
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
