$ErrorActionPreference = 'Stop'

$version = $env:VODLINK_OBS_VERSION
if ([string]::IsNullOrWhiteSpace($version)) { $version = '32.1.2' }

$root = Join-Path $env:GITHUB_WORKSPACE '.ci\obs'
$cache = Join-Path $env:GITHUB_WORKSPACE '.ci\obs-cache'
$extract = Join-Path $root 'extract'
$dev = Join-Path $root 'dev'
$runtime = Join-Path $root 'runtime'
$link = Join-Path $root 'link'

Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cache,$extract,$dev,$runtime,$link | Out-Null

function Invoke-Download($url, $out) {
  if (Test-Path $out) {
    Write-Host "Using cached $out"
    return
  }
  Write-Host "Downloading $url"
  Invoke-WebRequest -Uri $url -OutFile $out
}

$releaseBase = "https://github.com/obsproject/obs-studio/releases/download/$version"
$srcArchive = Join-Path $cache "OBS-Studio-$version-Sources.tar.gz"
$winArchive = Join-Path $cache "OBS-Studio-$version-Windows-x64.zip"
Invoke-Download "$releaseBase/OBS-Studio-$version-Sources.tar.gz" $srcArchive
Invoke-Download "$releaseBase/OBS-Studio-$version-Windows-x64.zip" $winArchive

$srcExtract = Join-Path $extract 'src'
New-Item -ItemType Directory -Force -Path $srcExtract | Out-Null
tar -xzf $srcArchive -C $srcExtract
$libobsSrc = Get-ChildItem $srcExtract -Recurse -Directory -Filter libobs | Select-Object -First 1
if (-not $libobsSrc) { throw 'Could not find libobs headers in OBS sources.' }

$includeRoot = Join-Path $dev 'include'
New-Item -ItemType Directory -Force -Path $includeRoot | Out-Null
Copy-Item -Recurse -Force $libobsSrc.FullName (Join-Path $includeRoot 'libobs')

# OBS' public libobs headers include SIMDe headers through libobs/util/sse-intrin.h.
# The official OBS binary packages do not ship those development headers, so CI must
# vendor a tiny private copy into the generated SDK include root. Keep it inside
# .ci\obs\dev so we never depend on a system OBS/dev installation.
$simdeVersion = $env:VODLINK_SIMDE_VERSION
if ([string]::IsNullOrWhiteSpace($simdeVersion)) { $simdeVersion = '0.8.2' }
$simdeArchive = Join-Path $cache "simde-$simdeVersion.tar.gz"
Invoke-Download "https://github.com/simd-everywhere/simde/archive/refs/tags/v$simdeVersion.tar.gz" $simdeArchive
$simdeExtract = Join-Path $extract 'simde'
Remove-Item -Recurse -Force $simdeExtract -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $simdeExtract | Out-Null
tar -xzf $simdeArchive -C $simdeExtract
$simdeSrc = Get-ChildItem $simdeExtract -Recurse -File -Filter sse2.h |
  Where-Object { $_.FullName -match '[\\/]simde[\\/]x86[\\/]sse2\.h$' } |
  Select-Object -First 1
if (-not $simdeSrc) { throw "Could not find SIMDe headers after extracting $simdeArchive." }
$simdeDir = Split-Path (Split-Path $simdeSrc.FullName -Parent) -Parent
$simdeDest = Join-Path $includeRoot 'simde'
Remove-Item -Recurse -Force $simdeDest -ErrorAction SilentlyContinue
Copy-Item -Recurse -Force $simdeDir $simdeDest

$obsConfig = Join-Path $includeRoot 'libobs\obsconfig.h'
if (!(Test-Path $obsConfig)) {
  @'
#pragma once
#define OBS_INSTALL_PREFIX ""
#define OBS_DATA_PATH "data/obs-studio"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELATIVE_PREFIX "../"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
#define OBS_BUILD_NUMBER 0
'@ | Set-Content -NoNewline -Encoding UTF8 $obsConfig
}

$winExtract = Join-Path $extract 'windows'
Expand-Archive -Path $winArchive -DestinationPath $winExtract -Force
$obsDll = Get-ChildItem $winExtract -Recurse -Filter obs.dll | Select-Object -First 1
if (-not $obsDll) { throw 'Could not find obs.dll in OBS portable zip.' }

$runtimeRoot = $obsDll.Directory.FullName
if ($obsDll.Directory.Name -ieq '64bit' -and $obsDll.Directory.Parent.Name -ieq 'bin') {
  $runtimeRoot = $obsDll.Directory.Parent.Parent.FullName
} elseif ($obsDll.Directory.Name -ieq 'bin') {
  $runtimeRoot = $obsDll.Directory.Parent.FullName
}

function Copy-FileIfExists($source, $destRelative) {
  if (!(Test-Path $source)) { return }
  $dest = Join-Path $runtime $destRelative
  $destDir = Split-Path $dest -Parent
  New-Item -ItemType Directory -Force -Path $destDir | Out-Null
  Copy-Item -Force $source $dest
}

function Copy-DirectoryIfExists($source, $destRelative) {
  if (!(Test-Path $source)) { return }
  $dest = Join-Path $runtime $destRelative
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Copy-Item -Recurse -Force (Join-Path $source '*') $dest
}

function Assert-FileExists($path, $message) {
  if (!(Test-Path $path)) {
    throw $message
  }
}

function Assert-DirectoryExists($path, $message) {
  if (!(Test-Path $path)) {
    throw $message
  }
}

function Assert-AnyFileExists($patterns, $message) {
  foreach ($pattern in $patterns) {
    if (Get-ChildItem -Path $runtime -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue | Select-Object -First 1) {
      return
    }
  }
  throw $message
}

function Copy-LibobsCoreDataDirectory($libobsSourceDirectory) {
  # Do not cherry-pick OBS core shaders. libobs' video initialization can add
  # or rename small data files between OBS point releases, and obs_reset_video()
  # expects the libobs data directory to be available as a coherent set.  Stage
  # the full OBS source-tree libobs/data directory into VodLink's private runtime
  # and then validate the important files below.  This is tiny compared with
  # QtWebEngine/OBS DLLs and avoids runtime "missing default.effect" bugs.
  $sourceData = Join-Path $libobsSourceDirectory 'data'
  if (!(Test-Path $sourceData)) {
    throw "Could not find OBS libobs/data directory at $sourceData."
  }

  $dest = Join-Path $runtime 'data\libobs'
  Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Copy-Item -Recurse -Force (Join-Path $sourceData '*') $dest
}

function Copy-MatchingFiles($sourceDir, $patterns, $destRelative) {
  if (!(Test-Path $sourceDir)) { return }
  foreach ($pattern in $patterns) {
    Get-ChildItem -Path $sourceDir -File -Filter $pattern -ErrorAction SilentlyContinue | ForEach-Object {
      Copy-FileIfExists $_.FullName (Join-Path $destRelative $_.Name)
    }
  }
}

function Resolve-SourceRuntimeFile($fileName) {
  $matches = Get-ChildItem -Path $runtimeRoot -Recurse -File -Filter $fileName -ErrorAction SilentlyContinue
  if ($matches) { return $matches[0].FullName }
  return $null
}

function Copy-RuntimeDependencyClosure {
  # The OBS portable ZIP has many plugin-specific DLL dependencies. Pattern-copying
  # only the obvious files can leave a plugin loadable enough to be found, but then
  # crash or fail when it tries to resolve a transitive DLL. Walk dumpbin /dependents
  # over every staged DLL/EXE and copy dependencies that exist inside the private OBS
  # portable tree. Windows system DLLs are intentionally not bundled.
  $copied = $true
  $passes = 0
  while ($copied -and $passes -lt 12) {
    $copied = $false
    $passes++
    $staged = Get-ChildItem -Path $runtime -Recurse -File -Include *.dll,*.exe -ErrorAction SilentlyContinue
    foreach ($file in $staged) {
      $deps = & dumpbin /dependents $file.FullName 2>$null
      foreach ($line in $deps) {
        if ($line -match '^\s*([A-Za-z0-9_.+-]+\.dll)\s*$') {
          $depName = $Matches[1]
          if ($depName -match '^(api-ms-|ext-ms-|kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|uuid|ws2_32|bcrypt|crypt32|secur32|shlwapi|version|winmm|dwmapi|d3d11|d3d12|dxgi|dxguid|mf|mfplat|mfreadwrite|propsys|setupapi|cfgmgr32|ntdll|ucrtbase|vcruntime|msvcp)') {
            continue
          }
          if (Get-ChildItem -Path $runtime -Recurse -File -Filter $depName -ErrorAction SilentlyContinue) {
            continue
          }
          $src = Resolve-SourceRuntimeFile $depName
          if ($src) {
            # Keep dependencies beside obs.dll by default. Plugin DLLs already live
            # in obs-plugins/64bit, and AddDllDirectory includes both folders.
            Copy-FileIfExists $src (Join-Path 'bin\64bit' $depName)
            $copied = $true
          }
        }
      }
    }
  }
  if ($passes -ge 12) {
    Write-Warning 'Dependency closure reached pass limit; continuing with staged OBS runtime.'
  }
}

$bin64 = Join-Path $runtimeRoot 'bin\64bit'
$plugins64 = Join-Path $runtimeRoot 'obs-plugins\64bit'
$dataRoot = Join-Path $runtimeRoot 'data'

# Stage a private minimal OBS runtime. Copying the whole portable OBS package made
# the single Windows exe explode in size because it included OBS' own Qt/CEF UI,
# frontend tools, themes, locales, browser docks, and unused plugins. VodLink only
# needs libobs, the graphics module, RTMP output, capture/audio plugins, hardware
# encoder plugins, and their DLL/data dependencies.
Copy-MatchingFiles $bin64 @(
  'obs.dll', 'libobs-*.dll', 'd3dcompiler*.dll', 'dxcompiler*.dll', 'dxil*.dll', 'avcodec*.dll', 'avformat*.dll', 'avutil*.dll',
  'swresample*.dll', 'swscale*.dll', 'libcurl*.dll', 'curl*.dll', 'jansson*.dll',
  'zlib*.dll', 'zstd*.dll', 'libssl*.dll', 'libcrypto*.dll', 'ssl*.dll', 'crypto*.dll',
  'mbedtls*.dll', 'mbedcrypto*.dll', 'mbedx509*.dll', 'srt*.dll', 'rist*.dll',
  'librist*.dll', 'libmbed*.dll', 'w32-pthreads*.dll', 'pthread*.dll',
  'libgcc*.dll', 'libstdc++*.dll', 'vcruntime*.dll', 'msvcp*.dll') 'bin\64bit'

# OBS probes hardware encoders in crash-isolated subprocesses. The plugins look
# these helpers up beside the host executable, while the dynamic packaging step
# copies them there from this staged runtime. Omitting them makes obs-nvenc,
# obs-qsv11 and obs-ffmpeg/AMF silently register no hardware encoders.
Copy-MatchingFiles $bin64 @(
  'obs-nvenc-test.exe', 'obs-qsv-test.exe', 'obs-amf-test.exe') 'bin\64bit'

# Some OBS Windows packages keep helper DLLs next to plugins rather than bin.
Copy-MatchingFiles $plugins64 @(
  'obs-outputs.dll', 'rtmp-services.dll', 'obs-ffmpeg.dll', 'win-capture.dll', 'win-wasapi.dll',
  'obs-qsv*.dll', '*qsv*.dll', '*amf*.dll', '*nvenc*.dll', '*encoder*.dll',
  'graphics-hook*.dll') 'obs-plugins\64bit'

# Game capture needs its graphics hook helpers/data. These are small compared with
# the OBS UI/browser stack and are required for real game capture.
Copy-MatchingFiles $bin64 @('graphics-hook*.dll', 'graphics-hook*.exe', 'inject-helper*.exe') 'bin\64bit'

# Core libobs data is not optional. Copy the entire directory from the exact OBS
# source archive used for this build instead of attempting a fragile minimal list.
Copy-LibobsCoreDataDirectory $($libobsSrc.FullName)

$requiredCoreEffects = @(
  'default.effect',
  'default_rect.effect',
  'opaque.effect',
  'solid.effect',
  'repeat.effect',
  'format_conversion.effect',
  'bicubic_scale.effect',
  'lanczos_scale.effect',
  'area.effect',
  'bilinear_lowres_scale.effect',
  'premultiplied_alpha.effect'
)
Copy-DirectoryIfExists (Join-Path $dataRoot 'obs-plugins\obs-outputs') 'data\obs-plugins\obs-outputs'
Copy-DirectoryIfExists (Join-Path $dataRoot 'obs-plugins\rtmp-services') 'data\obs-plugins\rtmp-services'
Copy-DirectoryIfExists (Join-Path $dataRoot 'obs-plugins\obs-ffmpeg') 'data\obs-plugins\obs-ffmpeg'
Copy-DirectoryIfExists (Join-Path $dataRoot 'obs-plugins\win-capture') 'data\obs-plugins\win-capture'
Copy-DirectoryIfExists (Join-Path $dataRoot 'obs-plugins\win-wasapi') 'data\obs-plugins\win-wasapi'

Copy-RuntimeDependencyClosure

Assert-FileExists (Join-Path $runtime 'bin\64bit\obs.dll') 'Private OBS runtime is missing bin\64bit\obs.dll.'
Assert-FileExists (Join-Path $runtime 'bin\64bit\libobs-d3d11.dll') 'Private OBS runtime is missing bin\64bit\libobs-d3d11.dll.'
Assert-FileExists (Join-Path $runtime 'bin\64bit\obs-nvenc-test.exe') 'Private OBS runtime is missing obs-nvenc-test.exe, required for NVENC registration.'
Assert-FileExists (Join-Path $runtime 'bin\64bit\obs-qsv-test.exe') 'Private OBS runtime is missing obs-qsv-test.exe, required for QSV registration.'
Assert-FileExists (Join-Path $runtime 'bin\64bit\obs-amf-test.exe') 'Private OBS runtime is missing obs-amf-test.exe, required for AMF registration.'
Assert-AnyFileExists @('libobs-opengl.dll') 'Private OBS runtime is missing libobs-opengl.dll fallback graphics module.'
Assert-FileExists (Join-Path $runtime 'obs-plugins\64bit\obs-outputs.dll') 'Private OBS runtime is missing obs-outputs.dll, required for RTMP output.'
Assert-FileExists (Join-Path $runtime 'obs-plugins\64bit\rtmp-services.dll') 'Private OBS runtime is missing rtmp-services.dll, required for RTMP service settings.'
Assert-FileExists (Join-Path $runtime 'obs-plugins\64bit\obs-ffmpeg.dll') 'Private OBS runtime is missing obs-ffmpeg.dll, required for FFmpeg/AAC encoders and hardware encoders.'
Assert-FileExists (Join-Path $runtime 'obs-plugins\64bit\win-capture.dll') 'Private OBS runtime is missing win-capture.dll, required for Windows game/window/monitor capture.'
Assert-FileExists (Join-Path $runtime 'obs-plugins\64bit\win-wasapi.dll') 'Private OBS runtime is missing win-wasapi.dll, required for Windows audio capture.'
Assert-DirectoryExists (Join-Path $runtime 'data\libobs') 'Private OBS runtime is missing data\libobs core data directory.'
Assert-DirectoryExists (Join-Path $runtime 'data\obs-plugins\obs-outputs') 'Private OBS runtime is missing data for obs-outputs.'
Assert-DirectoryExists (Join-Path $runtime 'data\obs-plugins\rtmp-services') 'Private OBS runtime is missing data for rtmp-services.'
Assert-DirectoryExists (Join-Path $runtime 'data\obs-plugins\obs-ffmpeg') 'Private OBS runtime is missing data for obs-ffmpeg.'
Assert-DirectoryExists (Join-Path $runtime 'data\obs-plugins\win-capture') 'Private OBS runtime is missing data for win-capture.'
Assert-DirectoryExists (Join-Path $runtime 'data\obs-plugins\win-wasapi') 'Private OBS runtime is missing data for win-wasapi.'

foreach ($optionalFile in @('d3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll')) {
  $optionalPath = Join-Path $runtime (Join-Path 'bin\64bit' $optionalFile)
  if (!(Test-Path $optionalPath)) {
    Write-Warning "Private OBS runtime does not contain $optionalFile; Windows may provide it system-wide, but OBS graphics startup can fail on machines that do not."
  }
}

foreach ($effect in $requiredCoreEffects) {
  $effectPath = Join-Path $runtime (Join-Path 'data\libobs' $effect)
  if (!(Test-Path $effectPath)) {
    throw "Private OBS runtime is missing required libobs core data file: $effectPath"
  }
}

# Keep a tiny manifest in the runtime so support/debug output can show what was
# bundled without dumping the user's installed OBS paths.
Get-ChildItem -Path $runtime -Recurse -File | ForEach-Object {
  $_.FullName.Substring($runtime.Length + 1).Replace('\\','/')
} | Sort-Object | Set-Content -Encoding UTF8 (Join-Path $runtime 'vodlink-obs-runtime-files.txt')

$privateObsDll = Get-ChildItem $runtime -Recurse -Filter obs.dll | Select-Object -First 1
if (-not $privateObsDll) { throw 'Private OBS runtime copy does not contain obs.dll.' }

$defFile = Join-Path $link 'obs.def'
$libFile = Join-Path $link 'obs.lib'
$exports = & dumpbin /exports $privateObsDll.FullName
$names = New-Object System.Collections.Generic.List[string]
foreach ($line in $exports) {
  if ($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([^\s=]+)') {
    $name = $Matches[1]
    if ($name -and !$name.StartsWith('?') -and !$name.Contains('.')) {
      $names.Add($name)
    }
  }
}
if ($names.Count -eq 0) { throw 'Could not parse exports from obs.dll.' }
"LIBRARY obs.dll`nEXPORTS" | Set-Content -Encoding ASCII $defFile
$names | Sort-Object -Unique | Add-Content -Encoding ASCII $defFile
& lib /nologo /machine:x64 /def:$defFile /out:$libFile | Write-Host
if (!(Test-Path $libFile)) { throw 'Failed to generate obs.lib from obs.dll exports.' }

@(
  "LIBOBS_ROOT=$dev",
  "LIBOBS_INCLUDE_DIR=$(Join-Path $includeRoot 'libobs')",
  "LIBOBS_LIBRARY=$libFile",
  "LIBOBS_RUNTIME_LIBRARY=$($privateObsDll.FullName)",
  "VODLINK_OBS_RUNTIME_DIR=$runtime"
) | Add-Content -Encoding UTF8 $env:GITHUB_ENV

Write-Host "Prepared private OBS runtime: $runtime"
Write-Host "Prepared libobs headers: $(Join-Path $includeRoot 'libobs')"
Write-Host "Prepared SIMDe headers: $(Join-Path $includeRoot 'simde')"
Write-Host "Prepared libobs import lib: $libFile"
