# Builds Chromium Embedded Framework (CEF) from source on Windows x64 with
# proprietary_codecs=true and ffmpeg_branding=Chrome (H.264, AAC, MP4 support).
#
# Requirements:
#   - ~160-200 GB free disk space on -WorkDir drive (short path recommended, e.g. C:\cef-build)
#   - 16-32+ GB RAM
#   - Visual Studio 2022/2026 with Desktop C++ + Windows 10/11 SDK + ATL/MFC
#   - Git & Python 3 in PATH
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\build_cef_proprietary.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build_cef_proprietary.ps1 -WorkDir D:\cef-build -InstallLocal
#   powershell -ExecutionPolicy Bypass -File tools\build_cef_proprietary.ps1 -WorkDir D:\cef-build -DownloadOnly
#   powershell -ExecutionPolicy Bypass -File tools\build_cef_proprietary.ps1 -WorkDir D:\cef-build -BuildOnly -InstallLocal
param(
    [string]$WorkDir = 'C:\cef-build',
    [switch]$InstallLocal,
    [switch]$SkipSpaceCheck,
    [switch]$DownloadOnly,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
if ($DownloadOnly -and $BuildOnly) { throw 'Cannot combine -DownloadOnly and -BuildOnly.' }
$root = Split-Path $PSScriptRoot -Parent
$version = (Get-Content "$root\cef_version.txt" -TotalCount 1).Trim()

# Parse "<cef>+g<commit>+chromium-<major>.0.<branch>.<patch>"
if ($version -notmatch '\+g([0-9a-f]+)\+chromium-\d+\.\d+\.(\d+)\.\d+$') {
    throw "Cannot parse CEF commit and Chromium branch from cef_version.txt: '$version'"
}
$cefCommit = $matches[1]
$branch    = $matches[2]

Write-Host "Target CEF version : $version"
Write-Host "CEF commit / branch: $cefCommit / $branch"
Write-Host "Work directory     : $WorkDir"

# Check free disk space (~50 GB for -DownloadOnly, ~150+ GB for full build)
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$driveRoot = [System.IO.Path]::GetPathRoot((Resolve-Path $WorkDir).Path)
$drive = Get-PSDrive -Name $driveRoot.TrimEnd(':\')
$freeGB = [math]::Round($drive.Free / 1GB, 1)
$minGB = if ($DownloadOnly) { 50 } else { 150 }
Write-Host "Free space on $driveRoot : $freeGB GB (minimum required: $minGB GB)"
if (-not $SkipSpaceCheck -and $freeGB -lt $minGB) {
    throw "Need at least $minGB GB free on $driveRoot (found $freeGB GB). Pass -WorkDir to another drive or -SkipSpaceCheck to override."
}

$depotTools = Join-Path $WorkDir 'depot_tools'
$automate   = Join-Path $WorkDir 'automate-git.py'

# 1. Fetch depot_tools
if (-not (Test-Path (Join-Path $depotTools 'gclient.bat'))) {
    $zip = Join-Path $WorkDir 'depot_tools.zip'
    Write-Host 'Downloading depot_tools ...'
    curl.exe -fSL --retry 3 -o $zip 'https://storage.googleapis.com/chrome-infra/depot_tools.zip'
    if ($LASTEXITCODE -ne 0) { throw 'Failed to download depot_tools.zip' }
    New-Item -ItemType Directory -Force $depotTools | Out-Null
    tar -xf $zip -C $depotTools
    if ($LASTEXITCODE -ne 0) { throw 'Failed to extract depot_tools.zip' }
    Remove-Item $zip -Force
}

# 2. Fetch automate-git.py (skip download in -BuildOnly if already present for offline builds)
if (-not $BuildOnly -or -not (Test-Path $automate)) {
    Write-Host 'Downloading automate-git.py ...'
    curl.exe -fSL --retry 3 -o $automate 'https://bitbucket.org/chromiumembedded/cef/raw/master/tools/automate/automate-git.py'
    if ($LASTEXITCODE -ne 0) { throw 'Failed to download automate-git.py' }
}

# 3. Configure environment for local VS toolchain + proprietary codecs
$env:PATH = "$depotTools;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$env:GN_DEFINES = 'is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome'
$env:CEF_ARCHIVE_FORMAT = 'tar.bz2'

# 4. Run CEF automated build (release minimal x64 only, no debug/symbols/docs/history)
if ($DownloadOnly) {
    Write-Host 'Starting automate-git.py (-DownloadOnly: fetching ~30-40 GB source without compiling) ...'
} elseif ($BuildOnly) {
    Write-Host 'Starting automate-git.py (-BuildOnly: compiling existing source in -WorkDir) ...'
} else {
    Write-Host 'Starting automate-git.py (this will download ~30 GB and compile for several hours) ...'
}
$args = @(
    $automate,
    "--download-dir=$WorkDir",
    "--depot-tools-dir=$depotTools",
    "--branch=$branch",
    "--checkout=$cefCommit",
    '--x64-build',
    '--no-debug-build',
    '--minimal-distrib-only',
    '--no-distrib-docs',
    '--no-distrib-symbols',
    '--no-chromium-history'
)
if ($DownloadOnly) {
    $args += @('--no-build', '--no-distrib')
} elseif ($BuildOnly) {
    $args += @('--no-update', '--force-build')
} else {
    $args += '--force-build'
}
& python @args
if ($LASTEXITCODE -ne 0) { throw "automate-git.py failed with exit code $LASTEXITCODE" }

if ($DownloadOnly) {
    Write-Host ''
    Write-Host '== CEF source download complete =='
    Write-Host "Compile later with: powershell -ExecutionPolicy Bypass -File tools\build_cef_proprietary.ps1 -WorkDir `"$WorkDir`" -BuildOnly -InstallLocal"
    return
}

# 5. Locate output archive & print checksums
$distribDir = Join-Path $WorkDir 'chromium\src\cef\binary_distrib'
$archive = Get-ChildItem $distribDir -Filter 'cef_binary_*_windows64_minimal.tar.bz2' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $archive) { throw "Build finished, but no *_windows64_minimal.tar.bz2 found in $distribDir" }

$sha1 = (Get-FileHash $archive.FullName -Algorithm SHA1).Hash.ToLower()
Write-Host ''
Write-Host '== CEF build complete =='
Write-Host "Archive : $($archive.FullName)"
Write-Host "SHA-1   : $sha1"

if ($InstallLocal) {
    $dest = Join-Path $root 'third_party\cef'
    $extracted = Get-ChildItem $distribDir -Directory -Filter 'cef_binary_*_windows64_minimal' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $extracted) { throw "Extracted minimal distrib folder not found in $distribDir" }
    Write-Host "Installing into $dest ..."
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
    Copy-Item $extracted.FullName $dest -Recurse -Force
    Set-Content (Join-Path $dest '.version') $version
    Write-Host "Installed! Run 'powershell -ExecutionPolicy Bypass -File build.ps1 -Clean' to rebuild NeonNet with MP4/H.264/AAC."
}
