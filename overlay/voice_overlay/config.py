"""Configuration for the voice overlay, persisted as JSON.

Mirrors the lightweight dataclass-with-JSON approach used by the Overwatch
overlays so settings survive restarts and can be hand-edited if needed.
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path


def _config_dir() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home()))
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    directory = base / "MumbleVoiceOverlay"
    directory.mkdir(parents=True, exist_ok=True)
    return directory


CONFIG_PATH = _config_dir() / "config.json"


@dataclass
class Config:
    # --- IPC: must match the plugin (env MUMBLE_VOICE_OVERLAY_HOST/PORT) ---
    host: str = "127.0.0.1"
    port: int = 27812

    # --- Placement ---
    overlay_x: int = 48
    overlay_y: int = 120
    overlay_width: int = 360

    # --- Appearance ---
    opacity: float = 0.92
    accent_color: str = "#00E5FF"
    font_size: int = 13
    max_cards: int = 8

    # --- Behaviour ---
    # How long a speaker's card lingers after they stop talking, in ms.
    linger_ms: int = 1200
    # When locked, the overlay is click-through and shows only live speakers.
    # When unlocked, it is draggable and shows a placeholder so it can be moved.
    locked: bool = True
    # Show your own card when you talk (handy for confirming PTT is working).
    show_self: bool = True
    # Show each speaker's channel name on their card.
    show_channel: bool = True
    # Show each speaker's Mumble comment (HTML stripped) on their card.
    show_comment: bool = True

    def save(self) -> None:
        try:
            CONFIG_PATH.write_text(json.dumps(asdict(self), indent=2))
        except OSError:
            pass

    @classmethod
    def load(cls) -> "Config":
        try:
            if CONFIG_PATH.exists():
                data = json.loads(CONFIG_PATH.read_text())
                known = {f.name for f in cls.__dataclass_fields__.values()}
                return cls(**{k: v for k, v in data.items() if k in known})
        except (json.JSONDecodeError, OSError, TypeError):
            pass
        return cls()
