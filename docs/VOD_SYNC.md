# VOD sync contract

VodLink switches participant VODs by preserving the same real-world instant, not
by copying the visible timestamp from one YouTube video to another.

For a source VOD and a target VOD:

```text
absoluteMoment = source.startedAt + sourceCurrentOffsetSeconds
targetOffset   = absoluteMoment - target.startedAt
```

`targetOffset` is clamped to the target VOD duration. This means if your VOD is
at 03:00 and a friend's recording started 80 seconds later, their player opens at
01:40 and continues playback automatically.

The in-app player keeps one `QWebEngineView` alive for the whole viewer session
and uses a `lite-youtube-embed` style facade for linked participant VODs. Hidden
participant entries are cheap poster/facade elements, not live YouTube iframes,
so they are fast to paint and do not poison playback before the user switches to
them. When a POV becomes active, VodLink creates the real YouTube JS player only
for that VOD, seeks to `targetOffset`, and starts playback.

The sync path uses fractional seconds internally. Copied YouTube links are still
floored to whole seconds because YouTube watch URLs use integer `t=` offsets.

## Unknown / live friend durations

Friend VODs can arrive from the Worker while the friend is still streaming, so
`duration_ms` may be `0`. VodLink treats that as "unknown duration", not as a
1-second VOD. Sync still maps through the real UTC timestamp and lets YouTube's
player clamp naturally if the requested point is not available yet.
