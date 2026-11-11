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
3. **Extract.** The installer runs `%SystemRoot%\System32\tar.exe -x -f <zip> -C %LOCALAPPDATA%\Programs\NeonNet` (included with Windows 10 1803 and later), without a console window.
4. **Create shortcuts.** The Start menu shortcut `NeonNet.lnk` is always created. The desktop shortcut is created if chosen, and removed if not.
5. **Register with Settings > Apps.** The key written is `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NeonNet`:

   | Value | Contents |
   |---|---|
   | `DisplayName` | NeonNet Browser |
   | `DisplayVersion` | from `APP_VERSION` |
   | `DisplayIcon` | `NeonNet.exe,0` |
   | `InstallLocation` | install folder |
   | `UninstallString` | `"...\NeonNet.exe" --uninstall` |
   | `EstimatedSize` | folder size in KB |

6. **Clean up.** The temporary zip is deleted.

Extraction runs on a worker thread, so the UI stays responsive with an animated progress bar. The close button is disabled while it runs.

Everything lives under the user's profile (`HKCU`, `%LOCALAPPDATA%`), so the installer never asks for UAC or admin rights.

## Command-line options

| Option | Effect |
|---|---|
| `--extract-to=<folder>` | Only extracts the app into that folder, without shortcuts or registry entries. Exit code 0 means success. Useful for testing or a portable copy. |

To check an installer without installing:

```bash
NeonNet-Setup.exe --extract-to=C:\temp\neonnet-test
```

## Upgrading

Running a newer installer overwrites the files in the same folder. The user's profile and settings are not touched.

Files that existed in the old version but not in the new one are left behind. This is harmless; a future version could clear the folder before extracting if needed.

## Uninstall

Handled by `NeonNet.exe --uninstall`. See the Uninstall section of [Architecture](ARCHITECTURE.md).

## Code signing

The installer and `NeonNet.exe` are not signed yet, so Windows SmartScreen warns users. For wider distribution, sign `build/app/NeonNet.exe`, `build/app/NeonNet.dll` and `dist/NeonNet-Setup.exe` with a code signing certificate (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 <file>`).

Sign in this order:
1. Sign the files in `build/app` before the payload is zipped.
2. Sign the installer after the payload is appended.

Signing the installer before appending the payload invalidates the signature.
