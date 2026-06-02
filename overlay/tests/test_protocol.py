"""Unit tests for the Qt-free speaker model and helpers."""

import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from voice_overlay.protocol import SpeakerModel, strip_html  # noqa: E402


def _user(uid, name, **kw):
    msg = {"t": "user", "id": uid, "name": name, "channel": "Alpha", "channelId": 1}
    msg.update(kw)
    return json.dumps(msg)


def _talk(uid, state):
    return json.dumps({"t": "talk", "id": uid, "state": state})


def test_strip_html():
    assert strip_html("<b>hi</b>&nbsp;there") == "hi there"
    assert strip_html("<p>line<br>break</p>") == "line break"
    assert strip_html("") == ""


def test_talk_makes_speaker_active():
    model = SpeakerModel()
    model.ingest(_user(1, "Reaver"))
    result = model.ingest(_talk(1, "talking"))
    assert result is not None
    assert [s.user.id for s in result] == [1]
    assert result[0].state == "talking"
    assert result[0].user.channel == "Alpha"


def test_passive_clears_speaker():
    model = SpeakerModel()
    model.ingest(_user(1, "Reaver"))
    model.ingest(_talk(1, "talking"))
    result = model.ingest(_talk(1, "passive"))
    assert result == []


def test_duplicate_state_is_noop():
    model = SpeakerModel()
    model.ingest(_user(1, "Reaver"))
    model.ingest(_talk(1, "talking"))
    assert model.ingest(_talk(1, "talking")) is None


def test_unknown_user_still_appears():
    # A talk event can arrive before we've seen a 'user' message.
    model = SpeakerModel()
    result = model.ingest(_talk(7, "shouting"))
    assert result is not None
    assert result[0].user.id == 7
    assert result[0].state == "shouting"


def test_show_self_toggle():
    model = SpeakerModel(show_self=False)
    model.ingest(_user(42, "You", self=True))
    # The state change registers, but a hidden self never appears in the
    # visible snapshot.
    assert model.ingest(_talk(42, "talking")) == []
    assert model.active_speakers() == []

    model.show_self = True
    result = model.ingest(_talk(42, "talking"))  # state unchanged -> None
    assert result is None
    speakers = model.active_speakers()
    assert [s.user.id for s in speakers] == [42]
    assert speakers[0].is_self is True


def test_self_state_attached_to_self_card():
    model = SpeakerModel()
    model.ingest(_user(42, "You", self=True))
    model.ingest(_talk(42, "talking"))
    result = model.ingest(json.dumps({"t": "self", "id": 42, "muted": True, "deafened": False}))
    assert result is not None
    assert result[0].self_muted is True


def test_leave_removes_active_speaker():
    model = SpeakerModel()
    model.ingest(_user(1, "Reaver"))
    model.ingest(_talk(1, "talking"))
    result = model.ingest(json.dumps({"t": "leave", "id": 1}))
    assert result == []


def test_disconnect_clears_everything():
    model = SpeakerModel()
    model.ingest(_user(1, "Reaver"))
    model.ingest(_talk(1, "talking"))
    result = model.ingest(json.dumps({"t": "server", "event": "disconnected"}))
    assert result == []
    assert model.active_speakers() == []


def test_ordering_self_last():
    model = SpeakerModel()
    model.ingest(_user(42, "You", self=True))
    model.ingest(_user(1, "Zara"))
    model.ingest(_user(2, "Alice"))
    model.ingest(_talk(42, "talking"))
    model.ingest(_talk(1, "talking"))
    speakers = model.ingest(_talk(2, "talking"))
    # Non-self sorted by name (Alice, Zara), self (You) last.
    assert [s.user.name for s in speakers] == ["Alice", "Zara", "You"]


def test_malformed_lines_ignored():
    model = SpeakerModel()
    assert model.ingest("not json") is None
    assert model.ingest("") is None
    assert model.ingest("[1,2,3]") is None
    assert model.ingest(json.dumps({"t": "unknown"})) is None
