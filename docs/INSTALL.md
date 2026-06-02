# Install

One DLL — that's it.

---

## Build the plugin

### Build (Windows, recommended for Star Citizen players)

Install [CMake](https://cmake.org/) and the Visual Studio C++ build tools, then:

```bat
cd plugin
cmake -S . -B build -A x64
cmake --build build --config Release
```

This produces `plugin\build\Release\mumble_voice_overlay.dll`.

Or use the shortcut from the repo root:

```powershell
.\build.ps1
```

This builds the DLL and copies it to your Desktop.

### Build (Linux/macOS, for development)

```bash
cd plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/mumble_voice_overlay.so (.dylib on macOS)
```

The overlay rendering is Windows-only (Win32 + GDI+). On Linux/macOS the
plugin compiles cleanly for CI but the overlay is a no-op.

---

## Install into Mumble

1. Open Mumble → **Configure → Settings → Plugins**.
2. Click **Install plugin…** and pick the built `mumble_voice_overlay.dll`.
3. Tick the checkbox next to **Mumble Voice Overlay (Star Citizen)** to enable
   it. (You do *not* need to enable "positional audio" — this plugin doesn't
   use it.)

Mumble ≥ 1.4 is required (the plugin uses the v1.0 plugin API).

> Leave Mumble's own **Overlay** feature **disabled** — that's the one EAC bans.
> This plugin replaces it without injecting into the game.

---

## Usage

The overlay appears automatically whenever someone in your channel talks.
Speaker cards stack in the top-left corner of your screen (configurable by
position constants in the source). When locked (the default), the overlay is
fully click-through so it never steals game input.

No separate overlay application is needed. No Python. No extra downloads.
