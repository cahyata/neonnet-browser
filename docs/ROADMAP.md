# Roadmap

Planned features for NeonNet, roughly in the order they are worth doing. Effort: **S** = a day or less, **M** = a few days, **L** = a week or more. A ticked box means it is already released.

## Done

- [x] Tabs, address bar, back/forward/reload, zoom, print, DevTools, fullscreen
- [x] Find in page (1.1.0)
- [x] Reopen closed tab (1.1.0)
- [x] Bookmarks and history pages (1.1.0)
- [x] Themed `≡` menu (1.1.0)
- [x] Automatic updates with checksum check (1.0.0, working since 1.1.2)

## Next: everyday use

- [ ] **Address bar suggestions** from history and bookmarks (M). Type a few letters, get matching pages and an inline completion. Most visible gap versus other browsers.
- [x] **Downloads page** (M, in the next release). Today a download only shows a status-bar line. Needs a list with progress, "open", "show in folder" and cancel, plus Ctrl+J.
- [x] **Choose the search engine** (S, in the next release). Plain text typed in the address bar goes to one fixed engine. Add a setting (for example Google, DuckDuckGo, Bing, Brave).
- [ ] **Settings page** (M). One place for home page, search engine, start-up behaviour, default download folder, scanlines and auto-update. Several of these are only in the menu or the registry now.
- [ ] **Clear browsing data** (S). Clear cookies, cache and site data (history already has its own button), with a time range.
- [ ] **Permission prompts** (M). Camera, microphone, location and notification requests need an in-app prompt with allow/block and a remembered choice.

## Tabs and windows

- [ ] **Drag to reorder tabs** (M).
- [ ] **Pin tabs** (S).
- [ ] **Restore tabs from the last session** (M). Needs care: `persist_session_cookies` must stay off (see the notes in [ARCHITECTURE.md](ARCHITECTURE.md)), so this has to be done by NeonNet itself, not by Chromium.
- [ ] **Private window** (M). A window with a temporary profile that is deleted on close.
- [ ] **Mute tab / audio indicator** (S).
- [ ] **Theme the right-click context menu** (M). It is still the native menu; the `≡` menu already has the NeonNet look.

## Bookmarks and history

- [ ] **Bookmark folders** (M).
- [ ] **Import bookmarks** from an HTML file exported by another browser (S).
- [ ] **Export bookmarks** to HTML (S).
- [ ] **Bookmarks bar** under the address bar (M).

## Security and privacy

- [ ] **Certificate error page** (S). Show a clear warning page with a "go back" default instead of the engine's page.
- [ ] **HTTPS-only mode** (S).
- [ ] **Tracker and ad blocking** (L). A built-in filter list, off by default or easy to toggle.
- [ ] **Saved passwords** (L). Only with encryption through Windows DPAPI; a poor implementation is worse than none.

## Platform

- [ ] **Set as default browser** (S). Register the URL and file handlers so links from other apps open in NeonNet.
- [ ] **Installer: choose install folder and per-machine install** (M).
- [ ] **Signed installer and executables** (M). Removes the SmartScreen warning. Needs a code-signing certificate.
- [ ] **Widevine DRM** (L). Needed for Netflix, Disney+ and similar; depends on a licence from the CDM vendor, so it may not be possible.
- [ ] **Extensions** (L). Currently turned off on purpose (`disable-extensions`); a full Chrome Web Store system is out of scope, but a small set of NeonNet-specific features could be.

## Quality and maintenance

- [ ] **Look into the speed gap with Chrome** (M). Speedometer 3.1 scored about 18 in NeonNet 1.1.2 against about 24 in Chrome 157. Field trials, GPU and build type are ruled out; the engine is one major version older. Updating Chromium is the first thing to try (see "Updating Chromium" in [MAINTENANCE.md](MAINTENANCE.md)).
- [ ] **Automated smoke test** (M). Start the app, load a page, close it and check the exit code is 0; this would have caught the crash on close earlier.
- [ ] **Update Chromium regularly** (S each time). Follow "Updating Chromium" in [MAINTENANCE.md](MAINTENANCE.md).
- [ ] **Crash log** (S). Write a short report when the app dies, so problems can be found without a debugger.
- [ ] **New tab page: stop the background animation when the tab is hidden** (S). The grid animation runs all the time.

## Suggested order

1. Address bar suggestions
2. Downloads page
3. Search engine and settings page
4. Drag-reorder and pinned tabs
5. Default browser registration
6. Certificate error page and permission prompts
7. Chromium update, then re-measure the speed gap
