# Mumble Voice Overlay — self-contained plugin

A [Mumble plugin](https://www.mumble.info/documentation/developer/positional-audio/create-plugin/)
that observes who is talking and renders an always-on-top overlay window
directly. One DLL, no separate overlay process, no Python, no extra downloads.

## Layout

- `src/voice_overlay_plugin.cpp` — the whole plugin (single file).
- `third_party/MumblePlugin.h` — Mumble's official plugin API header (BSD,
  vendored so the plugin builds without a Mumble checkout).
- `CMakeLists.txt` — builds the shared library.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -A x64 on Windows/MSVC
cmake --build build --config Release
```

Output: `mumble_voice_overlay.dll` (Windows) / `.so` (Linux) / `.dylib` (macOS).
Install it via **Mumble → Settings → Plugins → Install plugin…**.

On Linux/macOS the plugin compiles cleanly for CI but the overlay rendering is
a no-op (it uses Win32 + GDI+).

## Design notes

- **API version 1.0.0.** Every getter used here is part of the original plugin
  API, and the API struct is append-only, so the plugin loads on the widest
  range of Mumble releases (1.4.0+).
- **Overlay on a background thread.** The overlay window runs its own Win32
  message loop on a dedicated thread so it never blocks Mumble's UI.
- **Thread-safe.** Speaker state is guarded by a mutex. Mumble callbacks update
  the model and post a repaint message to the overlay thread.
- **No leaks.** Every string/array the API allocates is released via
  `api.freeMemory` immediately after copying it.
- **EAC-safe.** The overlay is an ordinary top-level OS window — no game process
  injection, no D3D/OpenGL hooking.
