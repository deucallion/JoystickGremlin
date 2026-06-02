"""Wire protocol parsing and the speaker state model.

This module is deliberately free of any Qt dependency so the core logic that
turns the plugin's JSON datagram stream into "who is currently speaking" can be
unit-tested on its own. See ../../docs/PROTOCOL.md for the message schema.
"""

from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field

PROTOCOL_VERSION = 1

# Talking states the plugin reports. Anything in ACTIVE_STATES means the user is
# currently producing (or attempting to produce) audio and should be shown.
ACTIVE_STATES = {"talking", "whispering", "shouting", "muted"}

_TAG_RE = re.compile(r"<[^>]+>")
_WS_RE = re.compile(r"\s+")


def strip_html(text: str) -> str:
    """Mumble comments are stored as HTML; reduce them to a flat snippet."""
    if not text:
        return ""
    flat = _TAG_RE.sub(" ", text)
    flat = (
        flat.replace("&nbsp;", " ")
        .replace("&amp;", "&")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", '"')
    )
    return _WS_RE.sub(" ", flat).strip()


@dataclass
class User:
    id: int
    name: str = ""
    channel: str = ""
    channel_id: int = -1
    comment: str = ""
    hash: str = ""
    locally_muted: bool = False
    is_self: bool = False


@dataclass
class Speaker:
    """A user the overlay is currently showing, with their live talk state."""

    user: User
    state: str
    is_self: bool
    self_muted: bool = False
    self_deafened: bool = False
    started_at: float = field(default_factory=time.monotonic)


class SpeakerModel:
    """Folds the datagram stream into the current set of active speakers.

    Feed it raw datagram lines via :meth:`ingest`; it returns the new ordered
    list of active speakers whenever that set (or its contents) changes, or
    ``None`` when the message had no visible effect.
    """

    def __init__(self, *, show_self: bool = True) -> None:
        self.show_self = show_self
        self.users: dict[int, User] = {}
        self.states: dict[int, str] = {}
        self.local_id: int | None = None
        self.self_muted = False
        self.self_deafened = False
        self.connected = False

    # -- ingestion ---------------------------------------------------------

    def ingest(self, line: str) -> list[Speaker] | None:
        line = line.strip()
        if not line:
            return None
        try:
            msg = json.loads(line)
        except (json.JSONDecodeError, ValueError):
            return None
        if not isinstance(msg, dict):
            return None

        kind = msg.get("t")
        handler = {
            "user": self._on_user,
            "talk": self._on_talk,
            "leave": self._on_leave,
            "self": self._on_self,
            "server": self._on_server,
            "meta": self._on_meta,
        }.get(kind)
        if handler is None:
            return None
        return handler(msg)

    def _on_user(self, msg: dict) -> list[Speaker] | None:
        uid = _as_int(msg.get("id"))
        if uid is None:
            return None
        user = User(
            id=uid,
            name=str(msg.get("name") or f"User {uid}"),
            channel=str(msg.get("channel") or ""),
            channel_id=_as_int(msg.get("channelId"), -1),
            comment=strip_html(str(msg.get("comment") or "")),
            hash=str(msg.get("hash") or ""),
            locally_muted=bool(msg.get("locallyMuted")),
            is_self=bool(msg.get("self")),
        )
        self.users[uid] = user
        if user.is_self:
            self.local_id = uid
        # Refresh details on any visible card without changing who is shown.
        return self._snapshot() if uid in self.states and self._is_active(uid) else None

    def _on_talk(self, msg: dict) -> list[Speaker] | None:
        uid = _as_int(msg.get("id"))
        if uid is None:
            return None
        state = str(msg.get("state") or "passive")
        previous = self.states.get(uid)
        self.states[uid] = state
        if previous == state:
            return None
        return self._snapshot()

    def _on_leave(self, msg: dict) -> list[Speaker] | None:
        uid = _as_int(msg.get("id"))
        if uid is None:
            return None
        was_active = self._is_active(uid)
        self.users.pop(uid, None)
        self.states.pop(uid, None)
        return self._snapshot() if was_active else None

    def _on_self(self, msg: dict) -> list[Speaker] | None:
        uid = _as_int(msg.get("id"))
        if uid is not None:
            self.local_id = uid
        self.self_muted = bool(msg.get("muted"))
        self.self_deafened = bool(msg.get("deafened"))
        # Mute/deaf badges only matter while our own card is showing.
        if self.local_id is not None and self._is_active(self.local_id):
            return self._snapshot()
        return None

    def _on_server(self, msg: dict) -> list[Speaker] | None:
        event = msg.get("event")
        if event == "disconnected":
            self.users.clear()
            self.states.clear()
            self.connected = False
            return []
        if event == "synchronized":
            self.connected = True
        return None

    def _on_meta(self, msg: dict) -> list[Speaker] | None:
        if msg.get("event") == "bye":
            self.users.clear()
            self.states.clear()
            return []
        return None

    # -- queries -----------------------------------------------------------

    def _is_active(self, uid: int) -> bool:
        if self.states.get(uid) not in ACTIVE_STATES:
            return False
        if not self.show_self and uid == self.local_id:
            return False
        return True

    def _snapshot(self) -> list[Speaker]:
        speakers: list[Speaker] = []
        for uid, state in self.states.items():
            if not self._is_active(uid):
                continue
            user = self.users.get(uid) or User(id=uid, name=f"User {uid}")
            speakers.append(
                Speaker(
                    user=user,
                    state=state,
                    is_self=(uid == self.local_id) or user.is_self,
                    self_muted=self.self_muted,
                    self_deafened=self.self_deafened,
                )
            )
        # Stable, readable ordering: yourself last, otherwise by name.
        speakers.sort(key=lambda s: (s.is_self, s.user.name.lower(), s.user.id))
        return speakers

    def active_speakers(self) -> list[Speaker]:
        return self._snapshot()


def _as_int(value: object, default: int | None = None) -> int | None:
    try:
        return int(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return default
