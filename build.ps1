# Builds NeonNet.
#   build\app\              runnable app folder (NeonNet.exe + NeonNet.dll + Chromium runtime)
#   dist\NeonNet-Setup.exe   installer (setup stub + zipped app folder appended)
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1 [-NoInstaller] [-Clean]
param([switch]$NoInstaller, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root
$cef = "$root\third_party\cef"

# --- MSVC environment (pick an install whose default toolset really has cl.exe) ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
foreach ($inst in (& $vswhere -all -products * -property installationPath)) {
    $verFile = Join-Path $inst 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (-not (Test-Path $verFile)) { continue }
    $ver = (Get-Content $verFile -TotalCount 1).Trim()
    if (Test-Path (Join-Path $inst "VC\Tools\MSVC\$ver\bin\Hostx64\x64\cl.exe")) { $vcvars = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat'; break }
}
if (-not $vcvars) { throw 'No working MSVC toolset found.' }
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }

function Run($exe, [string[]]$a) { & $exe @a; if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" } }

# --- 0. Chromium Embedded Framework (downloaded on demand, version pinned in cef_version.txt) ---
& "$root\tools\fetch_cef.ps1"
$cefVersion = (Get-Content "$root\cef_version.txt" -TotalCount 1).Trim()

if ($Clean) { Remove-Item -Recurse -Force build, dist -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force build\obj\wrapper, build\obj\app, build\app, dist | Out-Null
# a different CEF version needs a fresh wrapper library and runtime files
if ((Test-Path build\cef.version) -and ((Get-Content build\cef.version -TotalCount 1).Trim() -ne $cefVersion)) {
    Remove-Item -Recurse -Force build\libcef_dll_wrapper.lib, build\obj\wrapper, build\app -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force build\obj\wrapper, build\app | Out-Null
}
Set-Content build\cef.version $cefVersion
if (-not (Test-Path src\neonnet.ico)) { & "$root\tools\make_icon.ps1" -Out "$root\src\neonnet.ico" }

# Same defines/flags CEF's cmake uses for a Windows bootstrap (sandbox) build.
$defs = @('WIN32','_WIN32','_WINDOWS','UNICODE','_UNICODE','WINVER=0x0A00','_WIN32_WINNT=0x0A00','NTDDI_VERSION=NTDDI_WIN10_FE',
          'NOMINMAX','WIN32_LEAN_AND_MEAN','_HAS_EXCEPTIONS=0','NDEBUG','_NDEBUG','__STDC_CONSTANT_MACROS','__STDC_FORMAT_MACROS',
          'CEF_API_VERSION=CEF_API_VERSION_LAST','CEF_USE_SANDBOX','CEF_USE_BOOTSTRAP') | ForEach-Object { "/D$_" }
$cflags = @('/nologo','/c','/MP','/Gy','/GF','/GR-','/utf-8','/W3','/wd4100','/wd4127','/wd4244','/wd4324','/wd4996',
            '/std:c++20','/O2','/Ob2','/MT',"/I$cef",
            "/d1trimfile:$root") + $defs   # keep the local build path out of __FILE__ strings (no trailing \: it would escape the quote)

# --- 1. libcef_dll_wrapper.lib (built once) ---
$wrapperLib = 'build\libcef_dll_wrapper.lib'
if (-not (Test-Path $wrapperLib)) {
    Write-Host '== libcef_dll_wrapper (one time, ~1-2 min)'
    $srcs = Get-ChildItem "$cef\libcef_dll" -Recurse -Filter *.cc | ForEach-Object { $_.FullName }
    # object names must be unique: compile per directory into mirrored folders
    $groups = $srcs | Group-Object { Split-Path $_ -Parent }
    $objs = @()
    foreach ($grp in $groups) {
        $rel = $grp.Name.Substring($cef.Length + 1) -replace '[\\/]', '_'
        $od = "build\obj\wrapper\$rel\"
        New-Item -ItemType Directory -Force $od | Out-Null
        Run cl ($cflags + @('/DWRAPPING_CEF_SHARED', "/Fo$od") + $grp.Group)
        $objs += Get-ChildItem $od -Filter *.obj | ForEach-Object { $_.FullName }
    }
    Run lib (@('/nologo', "/OUT:$wrapperLib") + $objs)
}

# --- 2. NeonNet.dll ---
Write-Host '== NeonNet.dll'
Push-Location src; Run rc @('/nologo', '/fo', '..\build\obj\app\app.res', 'app.rc'); Pop-Location
Run cl ($cflags + @('/Fobuild\obj\app\', 'src\main.cpp', 'src\updater.cpp', 'src\store.cpp', 'src\perf.cpp'))
$libs = @('build\obj\app\main.obj', 'build\obj\app\updater.obj', 'build\obj\app\store.obj', 'build\obj\app\perf.obj', 'build\obj\app\app.res', $wrapperLib, "$cef\Release\libcef.lib",
          'comctl32.lib','crypt32.lib','delayimp.lib','gdi32.lib','rpcrt4.lib','shlwapi.lib','wintrust.lib','ws2_32.lib',
          'user32.lib','ole32.lib','shell32.lib','advapi32.lib','dwmapi.lib','uuid.lib','winhttp.lib','bcrypt.lib')
Remove-Item build\app\NeonNet.lib, build\app\NeonNet.exp -ErrorAction SilentlyContinue
# winhttp/bcrypt are only used by the updater in the browser process; delay-loading keeps them
# out of sandboxed sub-processes, which also load NeonNet.dll
Run link (@('/nologo','/DLL','/OUT:build\app\NeonNet.dll','/IMPLIB:build\obj\app\NeonNet.lib','/SUBSYSTEM:WINDOWS','/OPT:REF','/OPT:ICF',
            '/DELAYLOAD:libcef.dll','/DELAYLOAD:winhttp.dll','/DELAYLOAD:bcrypt.dll') + $libs)

# --- 3. runtime files + NeonNet.exe (renamed CEF bootstrap with our icon/version) ---
Write-Host '== app folder'
$bin = 'chrome_elf.dll','d3dcompiler_47.dll','dxcompiler.dll','libcef.dll','v8_context_snapshot.bin','vk_swiftshader.dll','vk_swiftshader_icd.json','vulkan-1.dll'
foreach ($f in $bin) {
    $s = "$cef\Release\$f"; $d = "build\app\$f"
    if (-not (Test-Path $d) -or (Get-Item $s).LastWriteTime -ne (Get-Item $d).LastWriteTime) { Copy-Item $s $d -Force }
}
Copy-Item "$cef\Resources\*" build\app -Recurse -Force
# license notices that must ship with the binaries
Copy-Item "$cef\LICENSE.txt" build\app\LICENSE-CEF.txt -Force
Copy-Item "$cef\CREDITS.html" build\app\CREDITS.html -Force
Copy-Item "$root\THIRD_PARTY_NOTICES.md" build\app\THIRD_PARTY_NOTICES.md -Force
if (Test-Path "$root\LICENSE") { Copy-Item "$root\LICENSE" build\app\LICENSE.txt -Force }
if (-not (Test-Path build\rescopy.exe)) {
    & cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE tools\rescopy.cpp /Fobuild\obj\ /Febuild\rescopy.exe user32.lib | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'rescopy build failed' }
}
Copy-Item "$cef\Release\bootstrap.exe" build\app\NeonNet.exe -Force
Run build\rescopy.exe @('build\app\NeonNet.dll', 'build\app\NeonNet.exe')

if ($NoInstaller) { return }

# --- 4. installer: setup stub + zip payload appended + footer ---
Write-Host '== installer'
Push-Location installer; Run rc @('/nologo', '/fo', '..\build\obj\setup.res', 'setup.rc'); Pop-Location
Run cl @('/nologo','/O2','/MT','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE','/Fobuild\obj\','/Febuild\setup-stub.exe',
         'installer\setup.cpp','build\obj\setup.res','/link','/SUBSYSTEM:WINDOWS',
         'user32.lib','gdi32.lib','ole32.lib','shell32.lib','shlwapi.lib','advapi32.lib','uuid.lib')
$zip = "$root\build\payload.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Push-Location build\app; Run tar @('-a', '-c', '-f', $zip, '*'); Pop-Location
$out = "$root\dist\NeonNet-Setup.exe"
$fs = [System.IO.File]::Create($out)
foreach ($part in "$root\build\setup-stub.exe", $zip) { $b = [System.IO.File]::OpenRead($part); $b.CopyTo($fs); $b.Close() }
$len = (Get-Item $zip).Length
$fs.Write([BitConverter]::GetBytes([UInt64]$len), 0, 8)
$fs.Write([System.Text.Encoding]::ASCII.GetBytes('NEONPAK1'), 0, 8)
$fs.Close()

# checksum file for the auto-updater; upload it to the release next to the installer
$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
[System.IO.File]::WriteAllText("$out.sha256", "$hash  NeonNet-Setup.exe`n", [System.Text.Encoding]::ASCII)

Get-Item build\app\NeonNet.exe, build\app\NeonNet.dll, $out, "$out.sha256" | Format-Table Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}}
