# Downloads the Chromium Embedded Framework build pinned in cef_version.txt
# (Windows 64-bit "minimal" distribution) into third_party\cef.
# The SHA-1 published in the official build index is verified before extracting.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\fetch_cef.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = (Get-Content "$root\cef_version.txt" -TotalCount 1).Trim()
$dest = "$root\third_party\cef"
$stamp = "$dest\.version"

if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp -TotalCount 1).Trim() -eq $version)) {
    Write-Host "CEF $version already present."
    return
}

Write-Host "Looking up CEF $version ..."
$index = Invoke-RestMethod 'https://cef-builds.spotifycdn.com/index.json'
$entry = $index.windows64.versions | Where-Object { $_.cef_version -eq $version } | Select-Object -First 1
if (-not $entry) { throw "CEF $version is not in the build index. Check cef_version.txt." }
$file = $entry.files | Where-Object { $_.type -eq 'minimal' } | Select-Object -First 1

New-Item -ItemType Directory -Force "$root\third_party" | Out-Null
$archive = "$root\third_party\cef-download.tar.bz2"
Write-Host "Downloading $($file.name) ($([math]::Round($file.size / 1MB)) MB) ..."
curl.exe -fSL --retry 3 -o $archive ('https://cef-builds.spotifycdn.com/' + [uri]::EscapeDataString($file.name))
if ($LASTEXITCODE -ne 0) { throw 'Download failed.' }

$sha = (Get-FileHash $archive -Algorithm SHA1).Hash.ToLower()
if ($sha -ne $file.sha1) { Remove-Item $archive; throw "SHA-1 mismatch (got $sha, expected $($file.sha1))." }

Write-Host 'Extracting ...'
$tmp = "$root\third_party\cef-extract"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null
tar -xjf $archive -C $tmp
if ($LASTEXITCODE -ne 0) { throw 'Extract failed.' }
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $dest
Remove-Item -Recurse -Force $tmp, $archive
Set-Content $stamp $version
Write-Host "CEF $version ready in third_party\cef"
