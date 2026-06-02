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

```
┌────────────┐   localhost UDP    ┌─────────────────────┐
│  Mumble    │   JSON datagrams   │  Voice Overlay app  │
│  + plugin  │ ─────────────────► │  (PyQt6, tray icon) │
│ (observes) │   :27812           │  draws over game    │
└────────────┘                    └─────────────────────┘
   no drawing                        no game hooks
```

- **`plugin/`** — a tiny native Mumble plugin (C++). It uses Mumble's plugin
  API to watch talking-state changes and gather each speaker's name, channel,
  comment, identity hash, and mute state, then fires newline-delimited JSON over
  localhost UDP. It never draws anything and never blocks Mumble.
- **`overlay/`** — a PyQt6 app that listens on that port and renders a stack of
  "speaker cards." Separate process, no injection → EAC-safe.
- **`tools/simulator.py`** — replays a fake Mumble session so you can see and
  position the overlay without connecting to a server.
- **`docs/`** — the [UDP protocol](docs/PROTOCOL.md) and
  [install guide](docs/INSTALL.md).

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

```bash
# 1. Build the plugin and install it via Mumble → Settings → Plugins
cd plugin && cmake -S . -B build && cmake --build build

# 2. Run the overlay (system tray app)
cd ../overlay && python -m pip install -r requirements.txt && python -m voice_overlay

# 3. (optional) See it without Mumble
python tools/simulator.py
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
