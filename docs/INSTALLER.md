# Installer

`NeonNet-Setup.exe` is a custom installer (`installer/setup.cpp`). It does not use NSIS, Inno Setup or MSI.

## File format

```
┌──────────────────────────┬──────────────────────────┬──────────────┬────────────┐
│ setup stub (exe, ~0.5MB) │ payload.zip (~180 MB)    │ zip size     │ "NEONPAK1" │
│                          │ contents of build/app    │ uint64 LE    │ 8 bytes    │
└──────────────────────────┴──────────────────────────┴──────────────┴────────────┘
```

- **How it is assembled:** `build.ps1` creates the zip with `tar -a -c -f payload.zip *`, then concatenates the stub, the zip and a 16-byte footer.
- **Why the payload is appended instead of being a resource:** a resource of several hundred MB is impractical for the resource compiler. Data appended after an exe is ignored by Windows when the program runs.
- **Integrity:** the `NEONPAK1` footer is checked before extracting. If it does not match, the installer shows "Setup is damaged".

## Install flow

1. **Check whether NeonNet is running.** The installer tries to open `NeonNet.exe` and `libcef.dll` in the target folder exclusively. If either is locked, it asks the user to close NeonNet.
2. **Copy the payload.** The zip is copied from the end of the exe to `%TEMP%\neonnet-payload.zip`.
3. **Extract.** The installer runs `%SystemRoot%\System32\tar.exe -x -f <zip> -C <install folder>` (included with Windows 10 1803 and later), without a console window. The folder is `%LOCALAPPDATA%\Programs\NeonNet` unless the user chose another one, or an earlier install registered one (running a newer installer by hand reuses it, and its kind of install).
4. **Create shortcuts.** The Start menu shortcut `NeonNet.lnk` is always created. The desktop shortcut is created if chosen, and removed if not. (For all users: the shared Start menu and the public desktop.)
5. **Register with Settings > Apps.** The key written is `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NeonNet`:

   | Value | Contents |
   |---|---|
   | `DisplayName` | NeonNet Browser |
   | `DisplayVersion` | from `APP_VERSION` |
   | `DisplayIcon` | `NeonNet.exe,0` |
   | `InstallLocation` | install folder |
   | `UninstallString` | `"...\NeonNet.exe" --uninstall` |
   | `EstimatedSize` | folder size in KB |

6. **Register as a web browser** (`src/browserreg.h`, shared with the app): the ProgIds `NeonNetURL` and `NeonNetHTML` under `Software\Classes`, the application entry `Software\Clients\StartMenuInternet\NeonNet` with its `Capabilities` (`URLAssociations` http and https, `FileAssociations` .htm .html .shtml .xht .xhtml), and the value `NeonNet` under `Software\RegisteredApplications`. This only lists NeonNet in Settings > Default apps; Windows leaves the choice to the user.
7. **Clean up.** The temporary zip is deleted.

Extraction runs on a worker thread, so the UI stays responsive with an animated progress bar. The close button is disabled while it runs.

By default everything lives under the user's profile (`HKCU`, `%LOCALAPPDATA%`), so the installer does not ask for UAC or admin rights.

### Choice of folder

**CHANGE FOLDER** opens the system's folder picker. The chosen folder goes through `ResolveDir()`: it must be a path on a drive; a folder that does not exist, is empty, or already holds a NeonNet is used as it is; any other folder gets a `NeonNet` folder inside it. This matters because uninstalling deletes the whole install folder. The same rule applies to `--dir=`.

### Install for all users

With **Install for all users** the install writes to `HKLM`, `Program Files` (the default folder), the shared Start menu and the public desktop, which needs administrator rights. The installer window runs as the normal user, so it starts a second copy of itself through `ShellExecuteEx` with the `runas` verb (Windows' UAC prompt): `--elevated --machine --dir="<folder>" --desktop=<0|1>`. That copy does the install without a window and its exit code is the `Err` of the install (10 NeonNet is running, 11 damaged, ... 17 permission not given, 18 could not start); the window shows the matching message. If the user declines the prompt, nothing is installed.

A copy installed for all users is not updated by NeonNet's updater (the updater would need administrator rights every time): `IsInstalledCopy()` is false for it, so it only reports new versions. A newer installer run by hand finds the existing install in `HKLM` and offers to update it in the same way.

## Command-line options

| Option | Effect |
|---|---|
| `--dir=<folder>` | The install folder (with `--silent`, for a scripted install). Passed through the folder rule above. |
| `--extract-to=<folder>` | Only extracts the app into that folder, without shortcuts or registry entries. Exit code 0 means success. Useful for testing or a portable copy. |
| `--silent` | Installs without any window. Used by the auto-updater. Keeps the desktop shortcut only if one already exists. Exit code 0 means success; on failure the reason is written to `HKCU\Software\NeonNet\UpdateResult`. |
| `--wait-pid=<pid>` | With `--silent`: first waits (up to 60 s) for that process to exit, then up to 30 s more for NeonNet's files to be released |
| `--relaunch` | With `--silent`: starts NeonNet after a successful install |

To check an installer without installing:

```bash
NeonNet-Setup.exe --extract-to=C:\temp\neonnet-test
```

## Upgrading

NeonNet's auto-updater downloads the newer installer and runs it with `--silent` after NeonNet closes (see [Architecture](ARCHITECTURE.md#auto-update)). Running a newer installer by hand does the same thing with the window. Either way, files in the same folder are overwritten, and the user's profile and settings are not touched.

Files that existed in the old version but not in the new one are left behind. This is harmless; a future version could clear the folder before extracting if needed.

## Uninstall

Handled by `NeonNet.exe --uninstall`. See the Uninstall section of [Architecture](ARCHITECTURE.md). For an install for all users, the program asks Windows for administrator rights first and runs again.

## Code signing

The installer and `NeonNet.exe` are not signed yet, so Windows SmartScreen warns users. For wider distribution, sign `build/app/NeonNet.exe`, `build/app/NeonNet.dll` and `dist/NeonNet-Setup.exe` with a code signing certificate (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 <file>`).

Sign in this order:
1. Sign the files in `build/app` before the payload is zipped.
2. Sign the installer after the payload is appended.

Signing the installer before appending the payload invalidates the signature.
