# CI OBS runtime

GitHub Actions must not use an OBS Studio installation from the runner. The build workflows run `scripts/ci/prepare-obs-runtime.*` before CMake.

The scripts download the pinned official OBS release assets for `VODLINK_OBS_VERSION`, currently `32.1.2`, into `.ci/obs-cache`, then prepare two separate paths:

- `LIBOBS_ROOT` / `LIBOBS_INCLUDE_DIR` / `LIBOBS_LIBRARY`: compile/link SDK paths used only by CMake.
- `VODLINK_OBS_RUNTIME_DIR`: the private runtime payload that VodLink embeds or bundles.

Platform behavior:

- Windows downloads the official portable ZIP, copies its runtime to `.ci/obs/runtime`, generates `obs.lib` from the private `obs.dll`, and embeds the runtime in `VodLink.exe` using native `RCDATA` resources instead of Qt `.qrc` resources.
- macOS downloads the official OBS `.dmg`, copies `OBS.app/Contents` as the private runtime, and bundles it inside `VodLink.app/Contents/Frameworks/obs-runtime`.
- Linux downloads the official Ubuntu 24.04 `.deb`, extracts it as the private runtime, copies the matching FFmpeg runtime libraries into that private runtime, and then copies it into `AppDir/usr/bin/obs-runtime` before producing the AppImage.

Do not add `Program Files/obs-studio`, `/Applications/OBS.app`, `/usr/lib/obs*`, or any user-installed OBS path to these workflows. CI should only use the private paths created by the prepare scripts.

## Shell script permissions

The macOS and Linux workflows invoke the OBS runtime preparer as `bash scripts/ci/prepare-obs-runtime.sh` instead of executing it directly. This avoids GitHub Actions failures when the executable bit is lost by a zip export, checkout option, or Windows-side edit. The script is still marked executable in the source package, but CI no longer depends on that bit.


### CI fixes: Windows import lib and Linux Qt Positioning plugins

Windows must link against the generated `obs.lib` import library, not directly
against `obs.dll`. The CI configure step passes `LIBOBS_LIBRARY` and
`LIBOBS_RUNTIME_LIBRARY` explicitly so `/DELAYLOAD:obs.dll` works without
feeding the DLL file to `link.exe`.

Linux does not use Qt Positioning/GPS plugins. The AppImage step removes Qt
position plugins before linuxdeploy runs; otherwise linuxdeploy can try to deploy
`libqtposition_nmea.so` and fail on the unused `libQt6SerialPort.so.6`
dependency.
