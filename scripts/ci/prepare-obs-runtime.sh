#!/usr/bin/env bash
set -euo pipefail

version="${VODLINK_OBS_VERSION:-32.1.2}"
root="${GITHUB_WORKSPACE}/.ci/obs"
cache="${GITHUB_WORKSPACE}/.ci/obs-cache"
extract="${root}/extract"
dev="${root}/dev"
runtime="${root}/runtime"

rm -rf "$root"
mkdir -p "$cache" "$extract" "$dev" "$runtime"

download() {
  local url="$1"
  local out="$2"
  if [[ -f "$out" ]]; then
    echo "Using cached $out"
    return
  fi
  echo "Downloading $url"
  curl -fL --retry 3 --retry-delay 5 -o "$out" "$url"
}

release_base="https://github.com/obsproject/obs-studio/releases/download/${version}"
src_archive="${cache}/OBS-Studio-${version}-Sources.tar.gz"
download "${release_base}/OBS-Studio-${version}-Sources.tar.gz" "$src_archive"

src_extract="${extract}/src"
mkdir -p "$src_extract"
tar -xzf "$src_archive" -C "$src_extract"
libobs_src="$(find "$src_extract" -type d -name libobs | head -n 1 || true)"
if [[ -z "$libobs_src" ]]; then
  echo "Could not find libobs headers in OBS sources." >&2
  exit 1
fi

include_root="${dev}/include"
mkdir -p "$include_root"
cp -a "$libobs_src" "${include_root}/libobs"

# OBS' public libobs headers include SIMDe headers through libobs/util/sse-intrin.h.
# The official OBS binary packages do not ship those development headers, so CI must
# vendor a tiny private copy into the generated SDK include root. Keep it inside
# .ci/obs/dev so we never depend on a system OBS/dev installation.
simde_version="${VODLINK_SIMDE_VERSION:-0.8.2}"
simde_archive="${cache}/simde-${simde_version}.tar.gz"
download "https://github.com/simd-everywhere/simde/archive/refs/tags/v${simde_version}.tar.gz" "$simde_archive"
simde_extract="${extract}/simde"
rm -rf "$simde_extract"
mkdir -p "$simde_extract"
tar -xzf "$simde_archive" -C "$simde_extract"
simde_sse2="$(find "$simde_extract" -type f -path '*/simde/x86/sse2.h' | head -n 1 || true)"
simde_src=""
if [[ -n "$simde_sse2" ]]; then
  simde_src="$(dirname "$(dirname "$simde_sse2")")"
fi
if [[ -z "$simde_src" || ! -f "$simde_src/x86/sse2.h" ]]; then
  echo "Could not find SIMDe headers after extracting ${simde_archive}." >&2
  find "$simde_extract" -maxdepth 4 -type f | sed -n '1,80p' >&2 || true
  exit 1
fi
rm -rf "${include_root}/simde"
cp -a "$simde_src" "${include_root}/simde"

if [[ ! -f "${include_root}/libobs/obsconfig.h" ]]; then
  cat > "${include_root}/libobs/obsconfig.h" <<'EOF'
#pragma once
#define OBS_INSTALL_PREFIX ""
#define OBS_DATA_PATH "data/obs-studio"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELATIVE_PREFIX "../"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
#define OBS_BUILD_NUMBER 0
EOF
fi

case "$(uname -s)" in
  Darwin)
    arch="$(uname -m)"
    if [[ "$arch" == "arm64" ]]; then
      mac_asset="OBS-Studio-${version}-macOS-Apple.dmg"
    else
      mac_asset="OBS-Studio-${version}-macOS-Intel.dmg"
    fi
    dmg="${cache}/${mac_asset}"
    download "${release_base}/${mac_asset}" "$dmg"
    mount_dir="${extract}/dmg"
    rm -rf "$mount_dir"
    mkdir -p "$mount_dir"
    hdiutil attach -nobrowse -quiet -mountpoint "$mount_dir" "$dmg"
    trap 'hdiutil detach -quiet "${mount_dir}" || true' EXIT
    obs_app="$(find "$mount_dir" -maxdepth 3 -type d -name 'OBS.app' | head -n 1 || true)"
    if [[ -z "$obs_app" ]]; then
      echo "Could not find OBS.app in ${dmg}." >&2
      exit 1
    fi
    cp -a "${obs_app}/Contents/." "$runtime/"

    # Trim OBS' own frontend/browser assets from VodLink's private runtime.
    # VodLink is the frontend; it only needs libobs, capture/output/audio/encoder
    # modules and their data. Keep Qt frameworks for compatibility with any OBS
    # plugin that still links Qt, but remove CEF/browser/frontend/theme/locale bulk.
    rm -rf \
      "$runtime/MacOS/OBS" \
      "$runtime/Frameworks/Chromium Embedded Framework.framework" \
      "$runtime/Resources/cef" \
      "$runtime/Resources/obs-browser" \
      "$runtime/Resources/locale" \
      "$runtime/Resources/themes" \
      "$runtime/Resources/frontend-tools" \
      "$runtime/PlugIns/obs-browser.plugin" \
      "$runtime/PlugIns/frontend-tools.plugin" \
      "$runtime/PlugIns/obs-websocket.plugin" \
      2>/dev/null || true

    libobs_lib="$(find "$runtime" -type f \( -path '*/libobs.framework/*/libobs' -o -path '*/libobs.framework/libobs' -o -name 'libobs*.dylib' \) | head -n 1 || true)"
    ;;
  Linux)
    deb="${cache}/OBS-Studio-${version}-Ubuntu-24.04-x86_64.deb"
    download "${release_base}/OBS-Studio-${version}-Ubuntu-24.04-x86_64.deb" "$deb"
    dpkg-deb -x "$deb" "$runtime"

    # The official OBS .deb links libobs against Ubuntu's FFmpeg shared libs,
    # but those packages are not embedded inside the OBS .deb itself. CI installs
    # them before this script runs; copy the exact runtime libs into VodLink's
    # private obs-runtime so the AppImage remains standalone and the linker can
    # resolve libobs' transitive references.
    ffmpeg_dest="${runtime}/usr/lib/x86_64-linux-gnu"
    mkdir -p "$ffmpeg_dest"
    for pattern in \
      /usr/lib/x86_64-linux-gnu/libavcodec.so.60* \
      /usr/lib/x86_64-linux-gnu/libavformat.so.60* \
      /usr/lib/x86_64-linux-gnu/libavutil.so.58* \
      /usr/lib/x86_64-linux-gnu/libswscale.so.7* \
      /usr/lib/x86_64-linux-gnu/libswresample.so.4*; do
      for lib in $pattern; do
        if [[ -e "$lib" ]]; then
          cp -a "$lib" "$ffmpeg_dest/"
        fi
      done
    done

    libobs_lib="$(find "$runtime" -type f -name 'libobs.so*' | head -n 1 || true)"
    ;;
  *)
    echo "Unsupported OS: $(uname -s)" >&2
    exit 1
    ;;
esac

# obs_reset_video() needs the core libobs shader/effect data files.  Do not
# cherry-pick them: copy the whole libobs/data directory from the exact OBS source
# archive used for this build, then validate the key files.  The directory is tiny
# compared with QtWebEngine/OBS binaries and avoids missing-shader runtime bugs.
if [[ ! -d "$libobs_src/data" ]]; then
  echo "Could not find OBS libobs/data directory at $libobs_src/data" >&2
  exit 1
fi
case "$(uname -s)" in
  Darwin)
    rm -rf "$runtime/Resources/data/libobs"
    mkdir -p "$runtime/Resources/data/libobs"
    cp -a "$libobs_src/data/." "$runtime/Resources/data/libobs/"
    ;;
  Linux)
    rm -rf "$runtime/usr/share/obs/libobs"
    mkdir -p "$runtime/usr/share/obs/libobs"
    cp -a "$libobs_src/data/." "$runtime/usr/share/obs/libobs/"
    ;;
esac
for effect in default.effect default_rect.effect opaque.effect solid.effect repeat.effect format_conversion.effect bicubic_scale.effect lanczos_scale.effect area.effect bilinear_lowres_scale.effect premultiplied_alpha.effect; do
  if ! find "$runtime" -type f -name "$effect" | grep -q .; then
    echo "Private OBS runtime is missing required libobs core data file: $effect" >&2
    exit 1
  fi
done

if [[ -z "${libobs_lib:-}" || ! -f "$libobs_lib" ]]; then
  echo "Could not find libobs library in private OBS runtime." >&2
  find "$runtime" -maxdepth 5 -type f | sed -n '1,120p' >&2 || true
  exit 1
fi

{
  echo "LIBOBS_ROOT=${dev}"
  echo "LIBOBS_INCLUDE_DIR=${include_root}/libobs"
  echo "LIBOBS_LIBRARY=${libobs_lib}"
  echo "VODLINK_OBS_RUNTIME_DIR=${runtime}"
} >> "$GITHUB_ENV"

echo "Prepared private OBS runtime: ${runtime}"
echo "Prepared libobs headers: ${include_root}/libobs"
echo "Prepared SIMDe headers: ${include_root}/simde"
echo "Prepared libobs library: ${libobs_lib}"
