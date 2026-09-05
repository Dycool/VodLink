# Rust Port Parity Contract

The acceptance baseline for the Rust port is the C++ `main` branch at commit
`a4ec966fb67473719ffaa31b22384981d0caa865`.

The Rust port is **not 100%** until an end user cannot tell which implementation
is running from visible behavior, persisted data, timing, capture output, or
normal application workflows.

## Hard invariants

- First-party Rust remains `#![forbid(unsafe_code)]`.
- `rust-port` is the only development branch for this port.
- CI is validation-only. It must never mutate repository contents.
- Existing `vodlink.db` data and settings remain compatible.
- Existing Google/YouTube/Worker behavior remains compatible.
- OBS capture must preserve the C++ privacy/audio/video semantics.
- Defaults, validation ranges, labels, dialogs, timing, and failure behavior are
  part of the compatibility contract, not implementation details.
- The desktop application must remain a desktop application. A browser tab is
  not an acceptable replacement for the C++ window.

## Differential parity checklist

### Desktop lifecycle
- [x] Dedicated 1580x900 application window with 1180x720 minimum.
- [x] `--minimized` / `--startup` starts without surfacing the main window when
      a tray is available.
- [x] Second instance activates the existing VodLink instance.
- [x] Closing the main window keeps VodLink running in the tray.
- [x] Tray exposes Open VodLink, Auto-record games, Share VODs with friends,
      Settings…, and Quit.
- [x] Quit requests stream finalization and retains the C++ 20-second bounded
      exit fallback.
- [x] First close notification and `tray_close_tip_shown` persistence exactly
      match the C++ client.
- [ ] Launch-at-startup persistence matches the C++ platform implementations.
- [ ] Release updater behavior matches the C++ client.

### Authentication gates
- [ ] First-run setup page visually matches the C++ setup page.
- [ ] Saved-token restore page and escape hatch visually/behaviorally match.
- [ ] Account menu, profile image, sign-out, and account switching match.

### Main library UI
- [ ] Main shell, collapsible friends drawer, header, footer, cards, statistics,
      search/filter/sort/order/visibility controls match.
- [ ] VOD thumbnail refresh/backoff behavior matches.
- [ ] Embedded viewer and processing/failed states match.
- [ ] Linked friend participants and synchronized timestamp switching match.
- [ ] Open-on-YouTube timestamp behavior matches.
- [ ] Own-VOD vs friend-VOD deletion prompts and behavior match.

### Settings
- [ ] Recorder settings dialog matches the C++ controls and immediate persistence.
- [ ] Resolution discovery/edit validation matches.
- [ ] YouTube bitrate recommendation behavior matches.
- [ ] Hardware encoder choices and conservative AV1 visibility match.
- [ ] Privacy labels and values match exactly.
- [ ] Microphone, notifications, launch-at-startup, add-game, sync, stop-stream,
      reset, and close behavior match.

### Game detection
- [x] First scan is baseline-only: already-running games do not create a launch edge.
- [x] Catalog-based detection and periodic installed-library refresh are present.
- [ ] Windows manual picker lists the same user-facing windowed processes.
- [ ] Linux Proton/Wine cmdline/cwd candidate detection matches.
- [ ] macOS process enumeration behavior is equivalent.
- [ ] Manual-add file chooser and OS-specific validation match.

### Streaming / YouTube / sharing
- [x] First-party OBS integration uses a pinned safe Rust wrapper and contains no
      first-party unsafe Rust.
- [ ] Capture-source fallback order, hardware encoder selection, HDR behavior,
      audio/microphone gain, and diagnostics are differential-tested against C++.
- [ ] YouTube broadcast creation/bind/live/complete/delete/sync metadata behavior
      is differential-tested against C++.
- [ ] 8-second ingest drain and cancellation semantics match.
- [ ] Worker start/stop friend matching and failure behavior match.

### Persistence / safety
- [x] SQLite schema/migrations retain compatibility with the C++ database.
- [x] `cargo clippy --all-targets -- -D warnings` is enforced in validation.
- [x] First-party lint overrides and unsafe escape hatches are rejected.
- [x] Miri runs against the first-party no-OBS core.
- [ ] Golden migration/database compatibility fixtures cover real C++ databases.
- [ ] Differential tests cover defaults and setting-key semantics.

This file is deliberately strict: checked items are implemented in the Rust
branch; unchecked items are blockers for claiming 100% parity.
