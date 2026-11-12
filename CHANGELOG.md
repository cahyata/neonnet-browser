# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
