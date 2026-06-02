# Mumble Voice Overlay (Star Citizen)

A **self-contained, EAC-safe voice overlay for Mumble**. Install one plugin DLL
into Mumble and it shows who is currently talking in an always-on-top window
over your game. No separate overlay app to download, no Python, no extra
processes.

## Why this exists

Mumble ships an excellent overlay, but it works by **injecting into the game's
rendering pipeline** (hooking OpenGL/Direct3D). Anti-cheat systems like Star
Citizen's **Easy Anti-Cheat (EAC)** treat that injection as tampering, so the
Mumble overlay simply doesn't show — and at worst trips anti-cheat.

This plugin takes the approach proven by the Overwatch tactical overlays:
draw a **separate, always-on-top transparent OS window** that the desktop
compositor paints over the (borderless) game. Nothing is injected into the game
process, so there's nothing for EAC to object to.

## How it works

```
┌────────────────────────────────────────────────┐
│  Mumble + plugin DLL                           │
│                                                │
│  observes talking state  ──►  renders overlay  │
│  (Mumble plugin API)        (Win32 + GDI+)     │
│                                                │
│  No game hooks. No injection. No extra process.│
└────────────────────────────────────────────────┘
```

Everything is in one DLL:
- **Observes** Mumble's talking state, channel roster, user details via the
  plugin API
- **Renders** speaker cards in a transparent always-on-top window using Win32
  layered windows and GDI+
- **No separate process** — the overlay runs on a background thread inside the
  plugin

## What a speaker card shows

Everything the Mumble plugin API exposes about a talker:

- **Name** (with a `(you)` tag for yourself)
- **Talking state** — TALKING / WHISPER / SHOUT / MIC MUTED, each colour-coded
- **Channel** they're speaking in
- **Comment** (their Mumble profile note, HTML stripped)
- **Status tags** — locally muted, self-muted, deafened

## Quick start

```bash
# Build the plugin DLL
cd plugin && cmake -S . -B build && cmake --build build

# Windows shortcut: from the repo root, run  .\build.ps1
# (builds the DLL and copies it to your Desktop)
```

Then install the DLL via **Mumble → Settings → Plugins → Install plugin**.
That's it — the overlay appears automatically when someone talks.

## Development

```bash
# plugin compile check (works on Linux/macOS too, overlay is a no-op stub)
cd plugin && cmake -S . -B build && cmake --build build
```

## License

MIT — see [LICENSE](LICENSE). Bundles Mumble's `MumblePlugin.h`
(`plugin/third_party/`, BSD-licensed by the Mumble developers).
