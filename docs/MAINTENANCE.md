# Maintenance and releases

## Updating Chromium

The Chromium inside NeonNet does not update itself. Chrome ships a stable security update most weeks (see the [Chrome Releases blog](https://chromereleases.googleblog.com/)), and a new stable version comes out about every 4 weeks. A browser that is rarely updated becomes easier to attack through malicious websites.

Recommended schedule:
- Update at least once per Chromium stable release (about every 4 weeks).
- Update right away when there is news of an actively exploited Chrome vulnerability ("zero-day").

Steps:

1. Open https://cef-builds.spotifycdn.com/index.html, choose **Windows 64-bit**, and find the newest build in the **Stable** channel.
2. Copy its full version string, for example `157.0.12+gabcdef0+chromium-157.0.8100.55`, into `cef_version.txt`.
3. Rebuild from scratch:

   ```bash
   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
   ```

   `fetch_cef.ps1` downloads the new version and verifies its SHA-1. `build.ps1` discards the old wrapper automatically.
4. If the compile fails, the CEF API changed. Check the headers in `third_party/cef/include` and adjust `src/main.cpp`. Handler signatures do change between versions; for example, `OnBeforePopup` now takes a `popup_id` parameter.
5. Test (see the checklist below), bump NeonNet's patch version, and release.

## Bumping the NeonNet version

The version number lives in three places. Change all of them:

| File | What to change |
|---|---|
| `src/resource.h` | `APP_VERSION` |
| `src/app.rc` | `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, `ProductVersion` |
| `installer/setup.rc` | Same as above |

Then add an entry to `CHANGELOG.md`.

Suggested scheme:
- **Patch** (1.0.**1**): Chromium update or bug fix.
- **Minor** (1.**1**.0): new features.
- **Major** (**2**.0.0): big changes.

## Release checklist

- [ ] `build.ps1 -Clean` finishes without errors
- [ ] `build/app/NeonNet.exe` opens and the new tab page looks right
- [ ] Heavy sites (YouTube, GitHub, Google Maps) work smoothly
- [ ] Tabs: create, switch, close, Ctrl+Tab, middle-click to close
- [ ] Back, forward, reload and home
- [ ] A non-existent address shows "SIGNAL LOST", and RETRY works
- [ ] A popup sign-in (for example "Sign in with Google") succeeds
- [ ] Video fullscreen can be entered and left, and F11 works
- [ ] Downloads show the save dialog and progress in the status bar
- [ ] The window can be moved, resized, maximized (double-click), minimized and closed
- [ ] Close and reopen: no stray Chromium windows, and the window position is remembered
- [ ] `NeonNet-Setup.exe --extract-to=<folder>` returns exit code 0, and the extracted app runs
- [ ] A real install, launch from the Start menu, then uninstall from Settings > Apps

## Publishing on GitHub

The repository is `neonnet-browser`. The app itself is still called NeonNet.

### What goes into the repository

Everything not listed in `.gitignore`: `src/`, `ui/`, `installer/`, `tools/`, `docs/`, `build.ps1`, `cef_version.txt`, `README.md`, `CHANGELOG.md`, `SECURITY.md`, `THIRD_PARTY_NOTICES.md`, `LICENSE` and `.gitignore`.

Keep out of the repository:

| Folder | Why |
|---|---|
| `third_party/cef/` | About 600 MB, and `fetch_cef.ps1` downloads it again |
| `build/`, `dist/` | Build output. `NeonNet-Setup.exe` (~180 MB) is also over GitHub's 100 MB file limit. |

### Creating a release (where the installer goes)

The installer is never committed. Upload it as a **Release** asset, which may be up to 2 GB per file:

1. Build with `build.ps1 -Clean` and go through the checklist above.
2. Tag the version:

   ```bash
   git tag v1.0.0
   ```

   ```bash
   git push origin v1.0.0
   ```

3. On GitHub: **Releases > Draft a new release**, pick the `v1.0.0` tag, write the release notes (copy from `CHANGELOG.md`), and upload `dist/NeonNet-Setup.exe`.
4. Mention the Chromium version (the contents of `cef_version.txt`) in the notes, so users know how recent the engine is.

With the GitHub CLI, step 3 is one command:

```bash
gh release create v1.0.0 dist/NeonNet-Setup.exe --title "NeonNet 1.0.0" --notes-file CHANGELOG.md
```

## License obligations when distributing

NeonNet distributes Chromium and CEF binaries, so their license notices must ship with it. `build.ps1` already copies `CREDITS.html` (licenses of every Chromium component), `LICENSE-CEF.txt`, `THIRD_PARTY_NOTICES.md` and NeonNet's own `LICENSE.txt` into the app folder. Do not remove them from `build/app`.

- **Trademarks:** "Chrome" and "Google Chrome" are Google trademarks. Do not use that name or logo for NeonNet. Saying "built on Chromium" is fine.
- **Codecs:** if you ever build CEF yourself with proprietary codecs (such as H.264 or AAC), check the codec licensing before distributing it.
