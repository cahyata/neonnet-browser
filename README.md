# NeonNet

A synthwave / cyber-HUD themed web browser for Windows. Dark purple, neon pink and cyan, clipped window corners, CRT scanlines, and a new tab page with a sunset and a moving grid.

NeonNet ships its own Chromium engine. It does not need Edge, WebView2 or any other runtime.

![New tab page](docs/images/new-tab.png)

![Browsing](docs/images/browsing.png)

## Features

- **Tabs:** open, close, switch, drag to reorder, pin, and open links in a new tab. Popups (for example sign-in windows) open as tabs. Optionally the tabs of the last session come back at start-up.
- **Address bar:** type an address or search words (Google search).
- **Theme:** clipped-corner window frame and a HUD-style status bar.
- **Scanlines:** a CRT line effect over every page, toggled with the `SCAN` button.
- **"SIGNAL LOST" error page:** shown when a site fails to load, with a RETRY button.
- **Standard browser features:** fullscreen (including video), zoom, print, downloads with a save dialog, and developer tools.
- **Privacy and security:** incognito window, clear browsing data, certificate warnings, HTTPS-only mode, tracker and ad blocking, and saved passwords locked with your Windows account.
- **Site permissions:** a lock icon in the address bar shows the connection state and lets you allow, block or be asked about camera, microphone, location and more, per site.
- **Smooth scrolling and network speed:** an animated mouse wheel and keyboard scroll, and live download and upload speed in the status bar, both with a switch in the settings.
- **Single window:** running `NeonNet.exe <url>` while NeonNet is open adds a tab to the existing window.
- **Chromium sandbox enabled:** renderer and GPU processes run isolated.
- **Installer:** per user by default (no admin rights needed), with a choice of folder and an option to install for all users. Uninstall from Settings > Apps. NeonNet can be set as the default browser.
- **Find in page, bookmarks and history:** Ctrl+F, Ctrl+D, Ctrl+H, Ctrl+Shift+O for bookmarks (with folders, bookmarks bar via Ctrl+Shift+B, and HTML import/export), and Ctrl+Shift+T to reopen a closed tab.
- **Incognito window:** Ctrl+Shift+N opens a window whose cookies, cache and sign-ins live in memory only and disappear when it closes. No history is recorded.
- **Performance panel:** Ctrl+Shift+M opens a side panel with memory and CPU per tab, a memory cleaner, sleeping tabs, and optional RAM, CPU and network download limiters.
- **Extensions (Userscripts):** Ctrl+Shift+E (`neonnet://extensions`) manages `.user.js` userscripts with `@match`/`@include`/`@exclude`, `@run-at`, and `GM_*` APIs.
- **Downloads page:** Ctrl+J lists every download with progress, cancel, open and show-in-folder.
- **Address bar suggestions:** matches from your bookmarks and history appear as you type, with inline completion.
- **Settings page:** Ctrl+, for the search engine, home page, download behaviour, scanlines and automatic updates.
- **Automatic updates:** new releases are downloaded in the background, checked against a SHA-256 checksum, and installed when NeonNet closes. Can be turned off.

## Download and install

Download `NeonNet-Setup.exe` from the Releases page, run it and click **INSTALL**. See the [user guide](docs/USER-GUIDE.md) for details.

Requirements: Windows 10 version 1803 or newer (64-bit), or Windows 11.

## Building from source

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```

Requires Visual Studio 2022 with the "Desktop development with C++" workload. The Chromium Embedded Framework is downloaded automatically on the first build. Full instructions are in [docs/BUILD.md](docs/BUILD.md).

## Documentation

| Document | Contents |
|---|---|
| [User guide](docs/USER-GUIDE.md) | Installing, features, shortcuts, data locations, uninstalling, FAQ |
| [Building](docs/BUILD.md) | Prerequisites, build steps, outputs, troubleshooting |
| [Architecture](docs/ARCHITECTURE.md) | How it works inside: processes, window, tabs, UI messaging |
| [Installer](docs/INSTALLER.md) | Installer format, install and uninstall flow |
| [Maintenance and releases](docs/MAINTENANCE.md) | Updating Chromium, release checklist, publishing on GitHub |
| [Changelog](CHANGELOG.md) | Version history |
| [Security policy](SECURITY.md) | How to report a vulnerability |
| [Third-party notices](THIRD_PARTY_NOTICES.md) | Third-party components and their licenses |

## Repository layout

```
neonnet-browser/
├─ src/
│  ├─ main.cpp          all browser logic
│  ├─ updater.cpp/.h    update check, download and checksum
│  ├─ store.cpp/.h      history, bookmarks (with folders & HTML import/export) and downloads
│  ├─ perf.cpp/.h       process memory/CPU sampling and limiters
│  ├─ crash.cpp/.h      unhandled exception and subprocess crash log
│  ├─ userscripts.cpp/.h  .user.js userscript parser, matcher and storage
│  ├─ browserreg.h      default browser registry helpers
│  ├─ resource.h        resource IDs and version
│  ├─ app.rc            icon, embedded UI pages, version info
│  ├─ app.manifest      Windows manifest (DPI awareness, common controls)
│  └─ neonnet.ico       application icon
├─ ui/
│  ├─ chrome.html       tab strip, address bar and bookmarks bar
│  ├─ newtab.html       new tab page
│  ├─ history.html      history page
│  ├─ downloads.html    downloads page
│  ├─ settings.html     settings page
│  ├─ bookmarks.html    bookmarks page
│  ├─ performance.html  performance panel
│  ├─ passwords.html    saved passwords page
│  ├─ permissions.html  site permissions page
│  ├─ extensions.html   extensions (userscripts) page
│  ├─ warning.html      certificate and HTTPS-only warning page
│  └─ blocklist.txt     tracker and ad domain list
├─ installer/
│  ├─ setup.cpp         installer
│  └─ setup.rc
├─ tools/
│  ├─ fetch_cef.ps1     downloads the Chromium Embedded Framework
│  ├─ rescopy.cpp       puts the icon and version info into NeonNet.exe
│  └─ make_icon.ps1     regenerates neonnet.ico
├─ docs/                documentation
├─ build.ps1            builds everything
└─ cef_version.txt      pinned Chromium Embedded Framework version
```

## Limitations

- DRM content (Netflix, Disney+, Spotify web player) does not play. NeonNet warns in the status bar when a site asks for it.
- H.264 and AAC are not built in (patent licences), so files such as most `.mp4` cannot play inside NeonNet. VP9, AV1, Opus and MP3 work, which covers YouTube. A video NeonNet cannot play can be opened in your own media player instead (see the user guide).
- No account sync or Chrome Web Store (`.crx`) extensions (NeonNet uses built-in `.user.js` userscripts instead).

## License

NeonNet is released under the [MIT License](LICENSE). Copyright (c) 2026 cahyata.

NeonNet is built on the Chromium Embedded Framework and Chromium, which have their own licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the notices are also installed with the app.
