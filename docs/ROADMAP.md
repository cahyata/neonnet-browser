# Roadmap

Planned features for NeonNet, roughly in the order they are worth doing. Effort: **S** = a day or less, **M** = a few days, **L** = a week or more. A ticked box means it is already released.

## Done

- [x] Tabs, address bar, back/forward/reload, zoom, print, DevTools, fullscreen
- [x] Find in page (1.1.0)
- [x] Reopen closed tab (1.1.0)
- [x] Bookmarks and history pages (1.1.0)
- [x] Themed `≡` menu (1.1.0)
- [x] Automatic updates with checksum check (1.0.0, working since 1.1.2)

Done without being planned here first (the planned ones are ticked in the lists below):

- [x] Short NeonNet addresses (`neonnet://settings`, `neonnet://history` ...) and a themed 404 page (1.2.0)
- [x] Home page setting, used by the home button and at start-up (1.2.0)
- [x] Ask-where-to-save switch for downloads (1.2.0)
- [x] Chromium's own pop-ups turned off where NeonNet shows the same thing itself: download bubble, link address bubble, zoom bubble (1.2.0)
- [x] Warning in the status bar when a site asks for DRM, and when a video cannot be played (1.3.0)
- [x] New right-click items: open link in incognito window, open image in new tab, view page source, search for the selected text (1.4.0)
- [x] History can be cleared by period from the settings page, with a link from the history page (1.4.0)
- [x] Tabs with sound, typed text or a kept-awake site are never put to sleep by the performance panel (1.5.0)
- [x] Site information panel (lock icon) with per-site permissions: location, camera, microphone, notifications, clipboard, sensors (1.6.0)
- [x] Smooth scrolling for the mouse wheel and the keyboard, with a setting (1.6.0)
- [x] Network speed (download and upload) at the bottom left of the status bar, with a setting (1.6.0)
- [x] Tab right-click menu: pin, duplicate, mute, close other tabs (1.6.0)

## Next: everyday use

- [x] **Address bar suggestions** from history and bookmarks (M, 1.2.0). Type a few letters, get matching pages and an inline completion. Most visible gap versus other browsers.
- [x] **Downloads page** (M, 1.2.0). Today a download only shows a status-bar line. Needs a list with progress, "open", "show in folder" and cancel, plus Ctrl+J.
- [x] **Choose the search engine** (S, 1.2.0). Plain text typed in the address bar goes to one fixed engine. Add a setting (for example Google, DuckDuckGo, Bing, Brave).
- [x] **Settings page** (M, 1.2.0). One place for home page, search engine, start-up behaviour, default download folder, scanlines and auto-update. Several of these are only in the menu or the registry now.
- [x] **Open unplayable videos in the system player, and warn about DRM** (M, 1.3.0). NeonNet has no H.264 or AAC (patent licences), so such videos are opened in the user's own player from a temporary copy that is deleted afterwards. The status bar warns when a site asks for DRM.
- [x] **Clear browsing data** (S, 1.4.0). Cookies, site data, cache and history, each for a chosen period.
- [x] **Permission prompts** (M, 1.6.0). Camera, microphone, location, notifications and similar requests show a NeonNet-themed prompt with allow and block. The choice is remembered per site and can be changed from the lock icon or `neonnet://permissions`. Page alerts and the leave-site question use the same themed window.
- [ ] **HTTP sign-in dialog** (M). Sites that ask for a user name and password (HTTP Basic) fail with SIGNAL LOST, because the engine never calls `GetAuthCredentials` in this build. Needs another way to get the credentials.

## Tabs and windows

- [x] **Drag to reorder tabs** (M, 1.6.0).
- [x] **Pin tabs** (S, 1.6.0). Right-click a tab. Pinned tabs sit at the left, are narrow and come back at the next start.
- [x] **Restore tabs from the last session** (M, 1.6.0). Done by NeonNet itself (`session.txt`), not by Chromium, because `persist_session_cookies` must stay off (see [ARCHITECTURE.md](ARCHITECTURE.md)). Off by default; pinned tabs always come back; restored tabs start asleep.
- [x] **Private window** (M, 1.4.0). Ctrl+Shift+N opens an incognito window with an in-memory profile.
- [x] **Mute tab / audio indicator** (S, 1.5.0). A speaker on the tab, click to mute. Sound made only through the Web Audio API is not detected.
- [x] **Performance panel** (L, 1.5.0). Ctrl+Shift+M: memory and CPU per tab, memory cleaner, sleeping tabs, RAM and CPU limiters, tabs with sound, typed text or a kept-awake site are never slept on their own.
- [ ] **Network limiter** (M). Cap the download speed of all tabs or of background tabs, through the DevTools network conditions.
- [x] **Theme the right-click context menu** (M, 1.4.0). The menu uses the same painted window as the `≡` menu.

## Bookmarks and history

- [x] **Bookmark folders** (M, unreleased).
- [x] **Import bookmarks** from an HTML file exported by another browser (S, unreleased).
- [x] **Export bookmarks** to HTML (S, unreleased).
- [x] **Bookmarks bar** under the address bar (M, unreleased).

## Security and privacy

- [x] **Certificate error page** (S, 1.6.0). A NeonNet warning with GO BACK as the default; the site can be opened anyway under ADVANCED, for the rest of the run.
- [x] **HTTPS-only mode** (S, 1.6.0). Off by default; asks before falling back to http.
- [x] **Tracker and ad blocking** (L, 1.6.0). About 82 000 domains from EasyList, EasyPrivacy and Peter Lowe, off by default, switch in settings and the menu. Not done: per-site exceptions, cosmetic (page element) filtering and rules with paths or scripts, which a full filter engine would add.
- [x] **Saved passwords** (L, 1.6.0). DPAPI, ask-to-save, fill only after a click, manager page. Not done: showing a password, import and export, a password check against leaked lists, and HTTP sign-in pop-ups (see the HTTP sign-in item under Next: everyday use).

## Platform

- [x] **Set as default browser** (S, unreleased). NeonNet registers its URL and file handlers and opens Windows' Default apps page; the choice itself stays with the user (Windows does not allow a program to choose itself).
- [x] **Installer: choose install folder and per-machine install** (M, unreleased). Not done: updating an install for all users by itself (it needs administrator rights each time).
- [ ] **Signed installer and executables** (M). Removes the SmartScreen warning. Needs a code-signing certificate.
- [ ] **Widevine DRM** (L). Needed for Netflix, Disney+ and similar; depends on a licence from the CDM vendor, and on H.264 and AAC, which are not built in because of patent licences. Not planned. NeonNet only warns in the status bar when a site asks for DRM, and opens videos it cannot play in the system player.
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
