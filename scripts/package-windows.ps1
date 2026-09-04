param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'target\release'
$dist = Join-Path $root 'dist'
$output = Join-Path $root 'installer-output'
$binary = Join-Path $release 'vodlink.exe'

if (!(Test-Path $binary)) {
    throw "Rust release binary was not found at $binary"
}

Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $output -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path $output | Out-Null

Copy-Item $binary (Join-Path $dist 'VodLink.exe') -Force

# cargo-obs-build stages the private OBS runtime beside the Rust binary. Copy
# only runtime payloads; never package Cargo intermediates such as deps/build.
$excludedDirectories = @('.fingerprint', 'build', 'deps', 'examples', 'incremental')
foreach ($entry in Get-ChildItem -LiteralPath $release -Force) {
    if ($entry.FullName -eq $binary) { continue }
    if ($entry.PSIsContainer) {
        if ($excludedDirectories -contains $entry.Name) { continue }
        Copy-Item $entry.FullName (Join-Path $dist $entry.Name) -Recurse -Force
        continue
    }

    $extension = $entry.Extension.ToLowerInvariant()
    if ($extension -in @('.dll', '.exe', '.json', '.pak', '.dat')) {
        Copy-Item $entry.FullName (Join-Path $dist $entry.Name) -Force
    }
}

$commit = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { 'local' }
Set-Content -NoNewline -Encoding ascii -Path (Join-Path $dist '.vodlink-build-commit') -Value $commit

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (!(Test-Path $iscc)) {
    throw "Inno Setup compiler was not found at $iscc"
}

& $iscc "/DMyAppVersion=$Version" (Join-Path $root 'installer\windows\VodLinkInstaller.iss')
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
}

$installer = Join-Path $output 'VodLink-Windows-x64-Setup.exe'
if (!(Test-Path $installer)) {
    throw "Expected installer was not created at $installer"
}

Write-Host "Created $installer"
