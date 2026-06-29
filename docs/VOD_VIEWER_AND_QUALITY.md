# VOD viewer overlay and quality-first streaming

## Viewer layout

The VOD viewer is no longer a side panel. The library grid remains mounted in the
main window and the viewer is shown as an overlay layer on top of that surface.
Closing the viewer hides only the overlay, preserving the library scroll/grid
state.

## Embedded YouTube player

The in-app player uses Qt WebEngine plus YouTube's IFrame Player API with:

- an HTTPS app origin (`https://vodlink.app/`) for `postMessage`/`origin`
- `enablejsapi=1`
- a request interceptor that sets a stable HTTP Referer for YouTube subrequests
- autoplay allowed by Qt WebEngine, with an overlay fallback if Chromium blocks
autoplay with audio
- memory cache so switching linked VODs does not recreate the WebView
- `suggestedQuality: highres` for quality-first playback

## Streaming quality preference

VodLink now requests `latencyPreference: normal` when creating YouTube broadcasts.
That avoids the low/ultra-low-latency tradeoffs that can reduce smoothness or cap
viewer resolution. Linked VOD sync still maps by real-world timestamps, and all
VodLink-created VODs use the same latency preference.

Encoder settings remain YouTube-compatible CBR with a 2-second keyframe interval,
but OBS encoder knobs now prefer a quality-balanced hardware path instead of
absolute max-quality settings that can steal frame time from the game. NVENC,
AMF, QSV, VideoToolbox and VAAPI are selected only on platforms where they make
sense, and full-resolution multipass/lookahead are avoided by default to keep
streaming overhead low.
