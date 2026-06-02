# Mumble Voice Overlay (Star Citizen)

An **EAC-safe voice overlay for Mumble**. It shows who is currently talking —
and as much as Mumble knows about them — in an always-on-top window over your
game.

## Why this exists

Mumble ships an excellent overlay, but it works by **injecting into the game's
rendering pipeline** (hooking OpenGL/Direct3D). Anti-cheat systems like Star
Citizen's **Easy Anti-Cheat (EAC)** treat that injection as tampering, so the
Mumble overlay simply doesn't show — and at worst trips anti-cheat.

This project takes the approach proven by the Overwatch tactical overlays:
draw a **separate, always-on-top transparent OS window** that the desktop
compositor paints over the (borderless) game. Nothing is injected into the game
process, so there's nothing for EAC to object to.

## How it works

The plugin draws its own transparent, always-on-top Win32 window directly —
no second app, no Python, no injection into the game process.

```
┌──────────────────────────────────────────────────────┐
│  Mumble + plugin DLL                                 │
│  ├─ observes talking state via Mumble plugin API     │
│  └─ draws overlay window with GDI+ (Win32 layered    │
│     window — lives in Mumble's process, NOT the game)│
└──────────────────────────────────────────────────────┘
```

- **`plugin/`** — the whole thing. One C++ file, one DLL. Watches who is
  talking via the Mumble plugin API, renders speaker cards into a GDI+ layered
  window (WS_EX_TOPMOST, WS_EX_TRANSPARENT when locked → never steals clicks).
- **`overlay/`** — kept as a Linux/macOS fallback. On those platforms the plugin
  streams state over UDP and the Python app draws the window instead.
- **`docs/`** — [install guide](docs/INSTALL.md) and [UDP protocol](docs/PROTOCOL.md).

## What a speaker card shows

Everything the Mumble plugin API exposes about a talker:

- **Name** (with a `(you)` tag for yourself)
- **Talking state** — TALKING / WHISPER / SHOUT / MIC MUTED, each colour-coded,
  with a live equaliser pulse
- **Channel** they're speaking in
- **Comment** (their Mumble profile note, HTML stripped)
- **Status tags** — locally muted, self-muted, deafened
- Stable identity (Mumble certificate **hash**) tracked under the hood

## Quick start

```powershell
# Windows (self-contained — just the DLL, no Python needed):
.\build.ps1
# Then: Mumble → Settings → Plugins → Install plugin... → pick DLL from Desktop
# Ctrl+Shift+V in-game to unlock/reposition. Right-click for options.
```

```bash
# Linux/macOS — build plugin, then run companion Python app:
cd plugin && cmake -S . -B build && cmake --build build
cd ../overlay && pip install -r requirements.txt && python -m voice_overlay
```

Full instructions: **[docs/INSTALL.md](docs/INSTALL.md)**.

## Development

```bash
# overlay unit tests (Qt-free, fast)
cd overlay && python -m pytest tests/

# plugin compile check
cd plugin && cmake -S . -B build && cmake --build build
```

The protocol is intentionally additive and forgiving — unknown message types
are ignored — so the plugin and overlay can evolve independently.

## License

MIT — see [LICENSE](LICENSE). Bundles Mumble's `MumblePlugin.h`
(`plugin/third_party/`, BSD-licensed by the Mumble developers).
