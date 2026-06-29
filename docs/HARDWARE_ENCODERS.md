# Hardware encoder policy

VodLink always prioritizes hardware video encoding. The settings dialog only shows codec labels that are backed by at least one hardware encoder registered by VodLink's private OBS runtime.

Displayed choices are codec-level labels, not implementation names:

- `H.264` appears only when a private hardware H.264 encoder is available.
- `HEVC` appears only when a private hardware HEVC encoder is available.
- `AV1` appears only when a private hardware AV1 encoder is available.

Examples:

- RTX 3070 Ti: should show H.264/HEVC through NVENC, but not AV1.
- RTX 40-series or newer with OBS AV1 NVENC support: should show AV1 as well.
- AMD/Intel GPUs: show only the codecs exposed by AMF/QSV/VAAPI/VideoToolbox in the bundled OBS runtime.

VodLink never uses `obs_x264`, generic software FFmpeg HEVC/AV1, or the user's installed OBS runtime as fallback. If the saved setting references a codec that is not available on the current machine, VodLink falls back to the first available private hardware codec. If no private hardware encoder exists, streaming fails with an explicit error instead of silently using CPU encoding.

On Windows, OBS discovers NVENC, QSV, and AMF capabilities through the
`obs-nvenc-test.exe`, `obs-qsv-test.exe`, and `obs-amf-test.exe` subprocesses.
Dynamic packages keep those probes and their private non-system dependencies
beside `VodLink.exe`, which is the lookup location used by OBS.

## Platform priority

The hardware priority queue is platform-gated so it does not waste time trying impossible backends:

- Windows: NVENC first, then AMD AMF, then Intel QSV.
- macOS: VideoToolbox only. Apple VideoToolbox is never probed on Windows/Linux, and AMF/NVENC/QSV/VAAPI are never preferred on macOS.
- Linux/AppImage: NVENC first, then VAAPI, then Intel QSV.

The queue is still filtered through OBS' registered encoder list from VodLink's private runtime. That means AV1 is hidden when the actual hardware/runtime does not expose a hardware AV1 encoder, even if the codec exists elsewhere on another GPU generation.

## Performance/quality balance

The default encoder knobs are tuned for good stream quality without stealing too much frame time from the game:

- YouTube-compatible CBR and 2-second GOP are kept.
- Hardware encoding is mandatory.
- Full-resolution multipass is not the default because it can cost extra GPU time.
- NVENC uses a quality-balanced preset with quarter-resolution multipass, psycho/spatial/temporal AQ enabled, and lookahead disabled.
- AMF/QSV/VideoToolbox/VAAPI use their platform-specific balanced/quality hardware paths and avoid expensive analysis/lookahead when available.

This is intentional: VodLink should keep streaming overhead low enough that the game does not start dropping frames just because recording is enabled.

HDR auto-mode requires a private hardware HEVC/Main10 encoder. If HDR is detected but HEVC hardware is missing, VodLink refuses software fallback because that would break the hardware-only policy and would not be a reliable HDR path.
