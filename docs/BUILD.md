# Building

## Prerequisites

| Component | Notes |
|---|---|
| Windows 10 1803 or newer, 64-bit | The scripts use the `tar.exe` and `curl.exe` that ship with Windows |
| Visual Studio 2022 (Community is fine) or Build Tools | **Desktop development with C++** workload, including the Windows 10/11 SDK |
| PowerShell 5.1 or 7 | Included with Windows |
| Internet connection | First build only: downloads the Chromium Embedded Framework (~175 MB) |
| Disk space | About 2 GB for CEF, build output and the installer |

CMake, Python and Ninja are not needed.

## Steps

```bash
git clone https://github.com/<username>/neonnet-browser.git
```

```bash
cd neonnet-browser
```

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```

The first build takes about 3–5 minutes: it downloads CEF, compiles `libcef_dll_wrapper`, then the app and the installer. Later builds take about 30–60 seconds.

## `build.ps1` options

| Option | Effect |
|---|---|
| (none) | Build the app and the installer |
| `-NoInstaller` | Build only the app folder. Faster for testing. |
| `-Clean` | Delete `build/` and `dist/`, then build from scratch |

## What `build.ps1` does

1. **Finds Visual Studio.** It uses `vswhere` to find an install whose default MSVC toolset actually contains `cl.exe`. This skips broken or half-installed Visual Studio setups.
2. **Downloads CEF.** It runs `tools/fetch_cef.ps1`, which downloads the version in `cef_version.txt`, verifies its SHA-1 and extracts it to `third_party/cef`. Skipped when that version is already there.
3. **Builds `libcef_dll_wrapper.lib`.** This is CEF's C++ layer over its C API. Built once per CEF version.
4. **Builds `NeonNet.dll`.** Compiled from `src/main.cpp`, with `ui/*.html` and the icon embedded as resources.
5. **Assembles the app folder `build/app`:**
   - the Chromium runtime (`libcef.dll`, `.pak` files, `locales/` and so on);
   - `NeonNet.exe`, which is CEF's `bootstrap.exe` renamed. `tools/rescopy.cpp` gives it NeonNet's icon and version info;
   - license files: `LICENSE.txt`, `CREDITS.html`, `LICENSE-CEF.txt` and `THIRD_PARTY_NOTICES.md`.
6. **Builds the installer.** `installer/setup.cpp` is compiled, `build/app` is zipped, and the zip is appended to the exe to produce `dist/NeonNet-Setup.exe`. Its SHA-256 goes into `dist/NeonNet-Setup.exe.sha256`, which the auto-updater checks downloads against.

The compiler flag `/d1trimfile` strips the local build path from source file names embedded in the binaries, so your folder names never end up in the released files.

## Output

| Path | Contents | Size |
|---|---|---|
| `build/app/` | Ready-to-run app: start `build/app/NeonNet.exe` | ~410 MB |
| `dist/NeonNet-Setup.exe` | The installer to distribute | ~180 MB |
| `dist/NeonNet-Setup.exe.sha256` | Checksum of the installer, uploaded with it | 84 bytes |

For a quick test without installing, run `build/app/NeonNet.exe` directly. It uses the same profile as the installed app (`%LOCALAPPDATA%\NeonNet\Profile`).

## Changing the look

| To change | Edit |
|---|---|
| Tab strip, address bar, buttons | `ui/chrome.html` |
| New tab page (sun, grid, quick links) | `ui/newtab.html` |
| Frame and status bar colors, row heights | Constants at the top of `src/main.cpp` (`C_BG`, `C_PINK`, `CHROME_DIP`, `STATUS_DIP`, ...) |
| Error page | `ErrorPageUrl` in `src/main.cpp` |
| Scanline effect on web pages | `ScanScript` in `src/main.cpp` |
| Icon | `tools/make_icon.ps1`, then delete `src/neonnet.ico` and rebuild |
| Installer look | `Paint` in `installer/setup.cpp` |
| Version number | `APP_VERSION` in `src/resource.h`, and the VERSIONINFO blocks in `src/app.rc` and `installer/setup.rc` |

The HTML files are embedded in `NeonNet.dll`, so every change needs a rebuild (`-NoInstaller` is enough for testing).

## Troubleshooting

| Problem | Fix |
|---|---|
| `No working MSVC toolset found` | Install the Desktop development with C++ workload in the Visual Studio Installer, or repair the install |
| `SHA-1 mismatch` while downloading CEF | The download was corrupted. Run again, or run `tools\fetch_cef.ps1 -Force` |
| `CEF ... is not in the build index` | The version in `cef_version.txt` was removed from the server. Move to a newer one (see [Maintenance](MAINTENANCE.md)) |
| `LNK1104: cannot open file 'build\app\NeonNet.dll'` | NeonNet from `build/app` is still running. Close it. |
| Compile errors after changing the CEF version | Delete `build/` (`build.ps1 -Clean`); the old wrapper does not match |
| Script blocked ("running scripts is disabled") | Run it through `powershell -ExecutionPolicy Bypass -File build.ps1` as shown above |
