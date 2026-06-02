# Install & run

Two pieces work together:

1. **The Mumble plugin** (`plugin/`) — a small native library Mumble loads. It
   observes who is talking and streams it over localhost UDP. It draws nothing.
2. **The overlay** (`overlay/`) — a Python app that listens and draws the
   always-on-top window over your game.

You need both running.

---

## 1. Build & install the Mumble plugin

### Build (Windows, recommended for Star Citizen players)

Install [CMake](https://cmake.org/) and the Visual Studio C++ build tools, then:

```bat
cd plugin
cmake -S . -B build -A x64
cmake --build build --config Release
```

This produces `plugin\build\Release\mumble_voice_overlay.dll`.

### Build (Linux/macOS, for development)

```bash
cd plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/mumble_voice_overlay.so (.dylib on macOS)
```

### Install into Mumble

1. Open Mumble → **Configure → Settings → Plugins**.
2. Click **Install plugin…** and pick the built `mumble_voice_overlay.dll`.
3. Tick the checkbox next to **Mumble Voice Overlay (Star Citizen)** to enable
   it. (You do *not* need to enable "positional audio" — this plugin doesn't
   use it.)

Mumble ≥ 1.4 is required (the plugin uses the v1.0 plugin API).

> Leave Mumble's own **Overlay** feature **disabled** — that's the one EAC bans.
> This plugin replaces it without injecting into the game.

---

## 2. Run the overlay

Requires Python 3.10+.

```bash
cd overlay
python -m pip install -r requirements.txt
python -m voice_overlay
```

A microphone icon appears in your system tray. Right-click it to:

- **Unlock (move/resize)** — show a draggable frame so you can position the
  overlay, then lock it again to make it click-through.
- **Show my own card** — toggle whether your own voice shows a card.
- **Quit**.

When locked (the default) the overlay is invisible and click-through until
someone talks, then speaker cards fade in over your game.

### Try it without Mumble

To position the overlay or just see it work, replay a fake session:

```bash
cd overlay
python tools/simulator.py
```

---

## Configuration

Settings live in a JSON file you can hand-edit:

- Windows: `%APPDATA%\MumbleVoiceOverlay\config.json`
- Linux/macOS: `~/.config/MumbleVoiceOverlay/config.json`

Key fields: `port`, `overlay_x`/`overlay_y`/`overlay_width`, `opacity`,
`accent_color`, `linger_ms`, `max_cards`, `show_channel`, `show_comment`,
`show_self`. See [PROTOCOL.md](PROTOCOL.md) for the data the overlay receives.

If you change the port, set the same value for Mumble by launching it with the
`MUMBLE_VOICE_OVERLAY_PORT` environment variable set.
