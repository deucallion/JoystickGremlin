# Mumble Voice Overlay — native plugin

A general-purpose [Mumble plugin](https://www.mumble.info/documentation/developer/positional-audio/create-plugin/)
that streams talking-state and user info to the out-of-process overlay over
localhost UDP. It implements only the observing half of Mumble's plugin API and
draws nothing.

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

## Design notes

- **API version 1.0.0.** Every getter used here is part of the original plugin
  API, and the API struct is append-only, so the plugin loads on the widest
  range of Mumble releases (1.4.0+).
- **Never blocks Mumble.** The UDP socket is non-blocking and all send errors
  are ignored — if the overlay isn't running, datagrams are simply dropped.
- **Thread-safe.** `onServerConnected`/`onServerDisconnected` run on a different
  thread from the talking/channel callbacks; shared state is guarded by a mutex,
  and API getters are only called from the main-thread callbacks where they're
  valid.
- **No leaks.** Every string/array the API allocates is released via
  `api.freeMemory` immediately after copying it.

See [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) for the message format.
