# VodLink

VodLink detects when a supported game starts, opens a YouTube Live broadcast, streams the session to YouTube through a private embedded libobs runtime, and keeps a searchable local library that links straight back to YouTube for playback and sharing. Written in C++20 with Qt 6.

## Current architecture

- **Private libobs runtime** - VodLink initializes libobs directly and creates its own private scene, sources, SDR/HDR-aware video encoders, AAC audio encoder, RTMP service, and RTMP output.
- **Standalone** - the app never searches for or uses the user's installed OBS Studio app, profile, scenes, plugins, cache, logs, or config folders.
- **Windows single-exe runtime extraction** - when `VODLINK_OBS_RUNTIME_DIR` is provided at build time, the OBS runtime files are embedded in `VodLink.exe` as Qt resources. At runtime they are extracted to `%LOCALAPPDATA%/VodLink/VodLink/obs-runtime`, then `obs.dll` is delay-loaded from that private directory.
- **macOS/Linux bundles** - macOS ships `obs-runtime` inside `VodLink.app/Contents/Frameworks`; Linux ships it inside the AppImage payload at `usr/bin/obs-runtime`. Nothing is expected next to the final `.app` or `.AppImage`.
- **YouTube RTMP/RTMPS** - YouTube still creates the Live Broadcast/Stream; libobs owns the local RTMP output.

## Why this does not conflict with real OBS

VodLink creates its own `obs-private` tree under the app-local data folder, passes `obs-private/modules` to `obs_startup`, sets OBS/XDG-related environment variables to the same private tree, only calls `obs_add_module_path` with paths under its private runtime, and never reads `%APPDATA%/obs-studio`, `~/Library/Application Support/obs-studio`, or `~/.config/obs-studio`. On Windows, `obs.dll` is delay-loaded only after VodLink extracts and registers its private runtime DLL directories.

## Streaming behaviour

- The selected recorder resolution is used as both OBS base canvas and output resolution. A `3440x1440` setting requests a `3440x1440` outgoing stream.
- HDR is automatic when VodLink can detect HDR output support. On Windows this uses DXGI HDR display detection and switches to P010/Rec.2100 PQ + HEVC/Main10. `VODLINK_FORCE_HDR=1` exists as a debug override.
- Game-only mode captures the desktop visually through OBS monitor/display capture, then places a black privacy mask over it whenever the detected game executable is not the foreground window. Audio stays OBS per-process game audio only.
- Game + external audio uses the same focus-gated desktop video, but captures system output audio plus the default microphone.
- Desktop captures the desktop visually with game-only audio. Desktop with external audio captures desktop video, system output audio, and the default microphone.

## Building

Requirements:

- CMake 3.25+
- C++20 compiler
- Qt 6.8+ with Widgets, Network, NetworkAuth, WebChannel, WebEngineWidgets, and Positioning
- libobs headers/import library from the same OBS build you bundle
- a private OBS runtime folder containing `obs.dll`/`libobs`, OBS graphics modules, `obs-plugins`, `data/libobs`, and `data/obs-plugins`

Example configure:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBOBS_ROOT=/path/to/obs-dev-root \
  -DVODLINK_OBS_RUNTIME_DIR=/path/to/private/obs-runtime
cmake --build build --config Release --parallel
```

On Windows, the runtime dir is embedded into the executable when `VODLINK_OBS_RUNTIME_DIR` is set. On macOS, the install/package step puts it inside `VodLink.app/Contents/Frameworks/obs-runtime`. On Linux, the AppImage step puts it inside the AppImage at `usr/bin/obs-runtime`.

## Source layout

```text
src/
  app/        - app state machine, auth wiring, YouTube session lifecycle
  auth/       - Google OAuth desktop flow
  cloud/      - friend/session Worker client
  games/      - foreground game detection
  library/    - local SQLite VOD library
  player/     - embedded YouTube playback/sync bridge
  streaming/  - libobs RTMP streamer and private OBS runtime bootstrap
  ui/         - Qt Widgets UI
  youtube/    - YouTube Data + Live Streaming API client
worker/       - Cloudflare Worker used only for blind friend/session matching
```


## GitHub Actions OBS runtime

CI automatically prepares the private OBS runtime before CMake by downloading the pinned official OBS release assets. See `docs/CI_OBS_RUNTIME.md`. The workflows should not be pointed at a user or runner OBS Studio install.
