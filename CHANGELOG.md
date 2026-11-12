# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Features
- Videos NeonNet cannot play (H.264 / AAC, such as most `.mp4` and `.mov` files) can be opened in your own media player. A direct link to such a file asks whether to open it in the system player, save it, or do nothing; on a page, clicking play on a video that cannot be played opens it in the system player (also videos that only load when played), and right-clicking a video adds **Open in system player**. The player gets a temporary copy, which NeonNet deletes once the player has closed it. NeonNet does not include or install any player or codec.
- A warning in the status bar when a site asks for DRM (Widevine, PlayReady, FairPlay), which NeonNet does not have, and when a video on a page fails because its format is not supported.

## [1.2.0] - 2026-11-12

### Features
- Downloads page (Ctrl+J, the `≡` menu, or the new tab page): every download with progress, speed and source; cancel, open and show in folder; search; remove rows or clear the list. The list is kept between runs. Programs and scripts cannot be opened from the page.
- Chromium's own download pop-up is turned off, because the downloads page replaces it.
- Address bar suggestions: bookmarks and history matches appear as you type (arrow keys, Enter, click, Esc), ranked by how well they match, how often and how recently you visited them, with inline completion of the best match. Nothing you type is sent anywhere for this.
- Settings page (Ctrl+, , the `≡` menu, or the new tab page): search engine (Google, DuckDuckGo, Bing, Brave or Ecosia), home page, whether to ask where to save each download (otherwise files go to the Downloads folder), scanlines and automatic updates.
- The home button and start-up use the home page from the settings.
- NeonNet's own pages show and accept short addresses in the address bar and on the command line: `neonnet://settings`, `neonnet://history`, `neonnet://bookmarks`, `neonnet://downloads`.
- An unknown NeonNet page shows a themed 404 page, and the tab is titled NOT FOUND.

### Fixes
- Zooming a NeonNet page (new tab, history, bookmarks, downloads) no longer zooms the toolbar. Chromium keeps zoom per site, and the toolbar shared one with those pages, so it shrank or was cut off. The toolbar now has its own address (chrome.neonnet.ui).
- NeonNet asks Windows for per-monitor DPI awareness itself, instead of relying on the engine to do it.
- The white link-address bubble and the zoom bubble from Chromium are turned off: NeonNet already shows both in its own status bar, so they appeared twice.
- The `≡` menu could open far to the left of its button on some screens. It is now placed from the right edge of the window, so a wrong screen-scale value can no longer move it by hundreds of pixels.

## [1.1.3] - 2026-11-12

### Fixes
- The loading bar animation now runs only while a page loads. It used to run all the time, using about 14% of a CPU core and some GPU even when idle.

## [1.1.2] - 2026-11-12

### Changes
- Version bump to test the automatic update from 1.1.1. No functional changes.

## [1.1.1] - 2026-11-12

### Fixes
- NeonNet no longer crashes when it is closed. The window was destroyed from inside the browser engine's close callback; it is now destroyed right after the callback returns.
- Automatic updates now install. The crash on close stopped the update installer from ever starting, and the installer is now also started before the engine shuts down.

## [1.1.0] - 2026-11-12

### Features
- Find in page: Ctrl+F opens a find bar with a match counter, next/previous (Enter, Shift+Enter, F3, Ctrl+G) and a match-case switch.
- Reopen closed tabs with Ctrl+Shift+T, at their old position, up to 25 tabs back.
- Bookmarks: the `☆` star in the address bar and Ctrl+D, a bookmarks page (Ctrl+Shift+O) with search, edit and delete, and the newest bookmarks on the new tab page.
- History: visits are kept for 90 days, with a history page (Ctrl+H) that can search, remove single visits or clear everything.
- New menu items for all of the above.
- The `≡` menu is drawn in the NeonNet theme (clipped corners, neon colors, scanlines) instead of the Windows menu style, and works with the arrow keys.

### Security
- NeonNet's own pages can no longer be loaded inside a frame by other sites.

## [1.0.0] - 2026-11-11

First release.

### Features
- Windows browser with a bundled Chromium 156 engine (through Chromium Embedded Framework 156.0.28), with the sandbox enabled.
- Synthwave / cyber-HUD theme: clipped-corner window frame, status bar, numbered tab strip, and a scanline effect that can be turned off.
- New tab page with a sunset, an animated grid, a search box and six quick links.
- "SIGNAL LOST" error page with a RETRY button, also used when a renderer crashes.
- Tabs; popups open as tabs (OAuth sign-ins keep working); "Open link in new tab"; middle-click to close a tab.
- Chrome-style keyboard shortcuts, the `≡` menu, zoom, print, fullscreen, downloads with a save dialog, and developer tools.
- Single window: starting NeonNet again with a URL adds a tab to the running window.
- Window position and the scanline setting are remembered.
- Per-user installer without admin rights, Start menu and desktop shortcuts, and uninstall from Settings > Apps (browsing data optionally removed too).
- Automatic updates from GitHub releases: background download, SHA-256 check, silent install when NeonNet closes, "Restart to update" in the menu, and an on/off switch.

### Known limitations
- No Widevine DRM, no extensions (extensions registered by other software are blocked), no find-in-page.
- Session cookies are not kept after the browser closes.
- Chromium processes take a few seconds to exit after the window closes.
