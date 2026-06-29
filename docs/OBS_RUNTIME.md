# Private OBS runtime packaging

VodLink must ship its own OBS runtime. Do not point `LIBOBS_ROOT`, `VODLINK_OBS_RUNTIME_DIR`, `PATH`, `DYLD_LIBRARY_PATH`, or `LD_LIBRARY_PATH` at a user's installed OBS Studio copy.

`LIBOBS_ROOT` is only the compile/link SDK root. `VODLINK_OBS_RUNTIME_DIR` is the VodLink-owned runtime payload that gets embedded/bundled into release artifacts.

## Windows

Build with:

```powershell
cmake -S . -B build -G Ninja `
  -DLIBOBS_ROOT="C:\vodlink-obs-dev" `
  -DVODLINK_OBS_RUNTIME_DIR="C:\vodlink-obs-runtime"
```

`VODLINK_OBS_RUNTIME_DIR` is embedded into `VodLink.exe` as native Windows `RCDATA` resources plus a small manifest. Runtime startup then does this order:

1. Extract the native embedded OBS runtime resources to `%LOCALAPPDATA%/VodLink/VodLink/obs-runtime`.
2. Create `%LOCALAPPDATA%/VodLink/VodLink/obs-private` with private module config, profile, plugin config, cache, and log folders.
3. Add only those private runtime DLL folders with `AddDllDirectory`.
4. Call the first libobs function, causing delay-loaded `obs.dll` to resolve from VodLink's extracted runtime.
5. Set OBS/XDG-related environment variables to VodLink-owned private folders before `obs_startup`.
6. Pass the private module config path to `obs_startup`.
7. Add only private plugin/data paths with `obs_add_module_path`.

This is intentionally not static linking. The DLLs are still DLLs; the exe is only the transport container. The runtime is not embedded with Qt `.qrc`, because a full OBS runtime generates a huge `qrc_*.cpp` file that can make MSVC run out of heap.

## macOS

The OBS runtime is bundled inside the `.app`, not next to it:

```text
VodLink.app/
  Contents/
    MacOS/vodlink
    Frameworks/obs-runtime/
      bin/ or lib/...
      obs-plugins/...
      data/...
```

The executable has an install rpath pointing at `@executable_path/../Frameworks/obs-runtime/...`, so `libobs` resolves from the `.app` payload before the app reaches `main()`. Runtime source/plugin/data lookup also uses `VodLink.app/Contents/Frameworks/obs-runtime`.

The build workflow verifies that the `.app` contains `Contents/Frameworks/obs-runtime` before signing and zipping it.

## Linux AppImage

The OBS runtime is bundled inside the AppImage payload, beside the actual executable inside `usr/bin`:

```text
AppDir/
  AppRun
  usr/bin/VodLink
  usr/bin/obs-runtime/
    bin/ or lib/...
    obs-plugins/...
    data/...
```

`VodLink` has an rpath for `$ORIGIN/obs-runtime/...`. The AppImage `AppRun` also sets `LD_LIBRARY_PATH` only to the bundled `usr/bin/obs-runtime` folders before launching VodLink. This helps libobs plugins resolve their private transitive libraries without touching a system OBS install.

## Minimum runtime contents

The exact folder shape depends on the OBS build, but the runtime must include the equivalent of:

```text
obs-runtime/
  bin/ or bin/64bit/             # obs.dll/libobs + graphics/audio/output dependencies
  lib/                           # Linux/macOS shared libraries when your OBS build puts them here
  obs-plugins/ or obs-plugins/64bit/
  data/libobs/
  data/obs-plugins/
```

Required modules for the current streamer:

- `obs-outputs` for `rtmp_output` and `rtmp_common`
- hardware video encoder modules only: NVENC, AMF, QSV, VideoToolbox or VAAPI. VodLink intentionally does not expose or fall back to `obs-x264`/software video encoders.
- an AAC encoder module, usually FFmpeg AAC/CoreAudio AAC inside OBS
- one capture module per OS: Windows win-capture, macOS ScreenCaptureKit/screen capture, Linux PipeWire/X11 capture

## HDR and audio behaviour

VodLink now chooses the OBS video color pipeline automatically at stream start:

- SDR/default: `NV12`, `Rec.709`, partial range, H.264.
- HDR detected: `P010`, `Rec.2100 PQ`, full range, HEVC/Main10 candidates first, and enhanced-RTMP/HDR flags on the OBS output settings.

Automatic HDR detection is currently strongest on Windows because DXGI exposes HDR output color space and luminance via `IDXGIOutput6::GetDesc1`. Linux/macOS still stay SDR unless `VODLINK_FORCE_HDR=1` is set, because there is no equally reliable cross-desktop HDR signal in this small embedded layer yet.

Audio is intentionally mode-based:

- `Game only`: captures the real desktop with OBS monitor/display capture to avoid fragile Game Capture hooks. A black color-source privacy mask is shown whenever the detected game executable is not the foreground window. Audio uses OBS `wasapi_process_output_capture` Application Audio only; it never falls back to desktop audio.
- `Game and external audio`: uses the same focus-gated desktop video, but captures system output audio and the default microphone so Discord/music/browser/mic/etc. are included.
- `Desktop`: captures the desktop visually without the focus-gate mask, while keeping audio to OBS per-process game audio only.
- `Desktop with external audio`: captures the desktop visually without the focus-gate mask and uses system output audio plus the default microphone.

All audio sources are explicitly assigned to OBS mixer 1 and monitoring is disabled so OBS does not play captured audio back to the user.

## YouTube recommended streaming profile

VodLink's OBS output is normalized to Google's current YouTube Live encoder guidance:

- Protocol: RTMPS when YouTube provides an RTMPS ingest URL; RTMP-compatible OBS output otherwise.
- Video: H.264 for SDR RTMP/RTMPS; automatic HDR uses HEVC/Main10 when the bundled OBS runtime exposes a compatible encoder.
- Frame rate: 30 or 60 fps, never above YouTube's 60 fps ingest guidance.
- Rate control: CBR.
- Keyframe interval: 2 seconds.
- Advanced video: progressive scan, square pixels, 2 B-frames, 1 reference frame, CABAC where the selected OBS encoder supports those properties.
- SDR: NV12, 8-bit, Rec.709, partial range.
- HDR: P010, 10-bit, Rec.2100 HLG, BT.2020 non-constant luminance metadata where the selected OBS encoder/output plugin supports those properties.
- Audio: AAC stereo, 128 Kbps, 44.1 kHz.
- Bitrate: stream start snaps stale saved configs to YouTube's recommended table for the selected resolution and frame rate. For HEVC/AV1-capable paths, the H.264 recommended value is clamped into Google's published HEVC/AV1 min/max range.

Note: YouTube's HDR help still says HLS is the safest/official HDR path for OBS HDR streams. VodLink keeps RTMPS/RTMP because the app's current live workflow is low-latency YouTube ingest; the HDR RTMP path is enabled only when the private OBS runtime supports enhanced RTMP/HEVC HDR.

## 2026-06 crash/size fix

Windows CI now stages a minimal private OBS runtime instead of embedding the whole
OBS portable package. The native resource payload is qCompress-compressed before it
is linked into `VodLink.exe`, then inflated into `%LOCALAPPDATA%/VodLink/.../obs-runtime`
on first use. This keeps the app standalone while avoiding OBS' frontend UI, CEF,
themes, locales, and unused plugins.

The Settings dialog deliberately does not start libobs just to populate the encoder
combo box. Runtime streaming still validates the actual private OBS hardware encoder
before going live. This avoids OBS startup/shutdown side effects when the user only
opens Settings.

OBS module loading is restricted to actual OBS plugin folders. Dependency folders
such as `bin/64bit`, `Frameworks`, and plain `usr/lib` are only used as DLL/dylib/so
search paths, not as module scan paths.
