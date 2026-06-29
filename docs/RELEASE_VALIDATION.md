# VodLink release validation

This release intentionally treats the OBS runtime as a private, versioned part of VodLink.
Do not depend on a user-installed OBS Studio folder and do not cherry-pick libobs shader files.

## Runtime layout

Windows embeds the private OBS runtime in `VodLink.exe` and extracts it to:

```text
%LOCALAPPDATA%\VodLink\obs-runtime
```

macOS bundles it at:

```text
VodLink.app/Contents/Frameworks/obs-runtime
```

Linux bundles it inside the AppImage at:

```text
AppDir/usr/bin/obs-runtime
```

The required core libobs data directory is copied as a complete directory from the exact OBS source archive used by CI:

```text
libobs/data -> obs-runtime/data/libobs            # Windows
libobs/data -> obs-runtime/Resources/data/libobs  # macOS
libobs/data -> obs-runtime/usr/share/obs/libobs   # Linux
```

## Build gates

The Windows CI and CMake configure step fail if the private runtime is missing the critical OBS files used by streaming startup:

- `obs.dll`
- `libobs-d3d11.dll`
- `obs-nvenc-test.exe`, `obs-qsv-test.exe`, and `obs-amf-test.exe`
- `obs-outputs.dll`
- `obs-ffmpeg.dll`
- `win-capture.dll`
- `win-wasapi.dll`
- `data/libobs/default.effect`
- `data/libobs/default_rect.effect`
- `data/libobs/format_conversion.effect`
- `data/libobs/premultiplied_alpha.effect`

At runtime, `ObsRuntime::validateRuntimeLayout()` checks the extracted/bundled OBS runtime before libobs tries to reset video.

The Windows dynamic artifact also places the three encoder probe executables
beside `VodLink.exe`. OBS resolves them from the host executable directory, not
from the private plugin directory; without these sidecars the hardware encoder
plugins load but register no NVENC, QSV, or AMF encoders.

## Streaming startup order

The streamer follows libobs frontend startup order:

1. prepare private OBS runtime and config/cache paths;
2. `obs_startup()`;
3. register libobs core data paths;
4. verify core `.effect` files;
5. `obs_reset_video()`;
6. `obs_reset_audio()`;
7. register paired module binary/data paths;
8. `obs_load_all_modules2()`;
9. `obs_post_load_modules()`;
10. create private scene/source/encoders/service/output;
11. `obs_output_start()`.

## AppImage gate

Linux release artifacts must copy `VodLink-x86_64.AppImage` explicitly. The workflow rejects artifacts smaller than 100 MB so it cannot accidentally upload `linuxdeploy-x86_64.AppImage` again.

## OBS frontend rules locked for release

VodLink must not start libobs just to populate Settings. The only streaming path
that initializes libobs is the recorder startup path, which follows OBS' documented
order: `obs_startup()`, `obs_reset_video()`, `obs_reset_audio()`, module path
registration/loading, `obs_post_load_modules()`, then source/encoder/output creation.

## Game only (Beta) validation

On Windows, `Game only (Beta)` uses OBS Application Audio (`wasapi_process_output_capture`) like Streamlabs/OBS: the video source and per-process audio source are separate. Validate that:

- the dynamic runtime bundles `win-wasapi.dll` and `data/obs-plugins/win-wasapi`;
- Settings shows `Game only (Beta)`, `Game and external audio`, and `Full desktop`;
- selecting `Game only (Beta)` creates `game_capture`/`window_capture` plus `wasapi_process_output_capture`, never `wasapi_output_capture`;
- when OBS lists the target game, both sources receive its exact `window` property value; otherwise Game Capture uses `any_fullscreen` while Application Audio retains the detected executable selector, visible in diagnostics/logs;
- if OBS has not listed the game window yet, Application Audio keeps the detected executable identity and binds when the window appears; if no executable hint exists or Application Audio is unavailable, VodLink fails stream start with a clear error instead of silently recording desktop audio.
