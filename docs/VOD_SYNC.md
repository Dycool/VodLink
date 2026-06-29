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
and now keeps a small LRU cache of YouTube IFrame players for linked participant
VODs. Hidden participant players are created as soon as the linked strip is
rebuilt, so switching avatars reuses a warm iframe/player instead of destroying
and recreating the embed every time.

The sync path uses fractional seconds internally. Copied YouTube links are still
floored to whole seconds because YouTube watch URLs use integer `t=` offsets.

## Unknown / live friend durations

Friend VODs can arrive from the Worker while the friend is still streaming, so
`duration_ms` may be `0`. VodLink treats that as "unknown duration", not as a
1-second VOD. Sync still maps through the real UTC timestamp and lets YouTube's
player clamp naturally if the requested point is not available yet.
