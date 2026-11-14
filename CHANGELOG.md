# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.9.0] - 2026-11-14

### Features
- **Theme Accent:** choose or customize the neon accent color in **Settings**, synced across the browser chrome and internal UI pages.
- **Custom Keybindings:** view and remap keyboard shortcuts in **Settings**, applied across browser chrome, tabs, and DevTools.
- **New Tab Scratchpad & Speed Dial:** persistent local quick-notes scratchpad and customizable Speed Dial tiles on `neonnet://newtab`.
- **Custom Search Engines & Address Bar Engine Selector:** add, edit, and remove custom search engines (`%s` / `{query}`) in **Settings**, and switch the active search engine directly from the address bar dropdown.
- **Command Palette Alias Indexing:** expanded keyword aliases (such as `dns`, settings, and action terms) for faster command and settings discovery.

### Fixes
- **Anti-white-flash & dark child windows:** override Chromium child HWND background brushes and keep the Performance panel hidden until loaded so opening popups or panels no longer flashes white.
- **Workspace & popup focus handling:** close open chrome popups (including Edit Workspace) when focus moves into a tab, and ensure global shortcuts (such as Command Palette) work reliably regardless of whether focus is in a tab or chrome.

## [1.7.0] - 2026-11-13

### Features
- **Extensions (Userscripts):** added a built-in `.user.js` userscript manager (`Ctrl+Shift+E`, `≡` > **Extensions (Userscripts)**, `neonnet://extensions`, also linked from **Settings > EXTENSIONS**). Supports `@name`, `@namespace`, `@version`, `@author`, `@description`, `@match`, `@include`, `@exclude`, `@run-at` (`document-start`, `document-end`, `document-idle`), `GM_addStyle`, `GM_getValue`/`GM_setValue`/`GM_deleteValue`/`GM_listValues`, `GM_openInTab`, `GM_info`, `unsafeWindow`, and Promise `GM.*` APIs, with file import, drag-and-drop, an inline editor, and read-only execution in incognito windows. Chrome Web Store `.crx` extensions remain disabled (`disable-extensions`) so Chrome's own windows do not appear outside NeonNet.
- **Bookmark folders, bar, and HTML import/export:** organize bookmarks into nested folders on `neonnet://bookmarks`, toggle the **Bookmarks bar** under the address bar (`Ctrl+Shift+B`, `≡` > **Show bookmarks bar**, or **Settings > START**), and import/export standard Netscape Bookmark HTML files.
- **Network limiter:** cap download speed (in `KB/s` or `MB/s`) for all tabs or only background tabs from the Performance panel (`Ctrl+Shift+M`), applied via DevTools `Network.emulateNetworkConditions` and remembered across restarts.
- **Crash log:** unhandled exceptions, `SIGABRT`, and unexpected renderer terminations are recorded to `%LOCALAPPDATA%\NeonNet\crash.log` (rotated to `crash.prev.log` above 256 KB) with timestamp, NeonNet/Chromium/Windows versions, exception details, and stack frames.
- **New tab page idle optimization:** pauses the background grid CSS animation and clock timer whenever the tab is hidden (`visibilitychange`).
- Set as default browser. **Settings > DEFAULT BROWSER** has a **SET AS DEFAULT** button: it adds NeonNet to Windows' list of browsers (the URL handlers for http and https and the file handlers for .htm, .html, .shtml, .xht and .xhtml) and opens Windows' Default apps page, where you choose it. Windows 10 and 11 do not let a program choose itself, so the last step is yours. The settings page says whether NeonNet is already the default. The installer registers NeonNet in the same way. Links and .html files from other programs open in NeonNet (in the running window as a new tab).
- Installer: **CHANGE FOLDER** picks the folder to install into. A folder that already holds other things gets a `NeonNet` folder inside it, so that uninstalling never removes someone else's files. Running a newer installer by hand keeps the folder of the earlier install.
- Installer: **Install for all users** (asks for administrator rights through Windows' UAC prompt). It installs into `Program Files`, puts the shortcuts in the shared Start menu and desktop, and registers NeonNet for the whole computer. Uninstalling such a copy also asks for administrator rights. A copy for all users does not update itself (it lives where the user cannot write): it only reports new versions.
- Installer: **Choose NeonNet as my default browser when done** opens Windows' Default apps page after installing.

### Fixes
- Uninstalling no longer leaves the program folder behind when the final "NeonNet has been removed" message is read for more than a few seconds.

## [1.6.1] - 2026-11-13

### Fixes
- Closing the developer tools with their own X button no longer closes the whole browser. The button now closes only the developer tools (F12 did already).

## [1.6.0] - 2026-11-13

### Features
- Pin tabs: right-click a tab and choose **Pin tab**. A pinned tab sits at the left of the strip, is narrow (a pin and its number) and has no close button (Ctrl+W still closes it). Pinned tabs stay together at the left, and come back at the next start.
- Drag a tab to reorder it. The tab follows the mouse and the others make room. Pinned tabs move among the pinned ones, the others among the others.
- The tab right-click menu also has **Duplicate tab**, **Mute tab** (for a tab with sound), **Close other tabs** (pinned tabs stay) and **Close tab**.
- Developer tools (F12 or right-click > Inspect) open docked at the bottom of the NeonNet window, below the page, instead of in a separate window. They belong to the tab they inspect: another tab hides them, F12 again closes them. Window shortcuts pressed inside them (new tab, new incognito window, reopen tab, history, downloads, settings, next and previous tab, fullscreen) work as in the page instead of opening a Chromium window. The three-dot menu of the tools has a working **Dock side** (bottom, right, left, separate window) as in Chrome and Edge, and the choice is remembered; **Settings > DEVELOPER TOOLS** sets the same position. The device toolbar (phone and tablet sizes, Ctrl+Shift+M) works.
- Site information and permissions: a lock icon at the left of the address bar opens a panel with the connection state and, per site, Ask / Allow / Block for location, camera, microphone, notifications, clipboard and motion sensors. Answers to permission prompts are now remembered per site (in `sitepermissions.txt`, plain text; in memory only in an incognito window) and are listed in **neonnet://permissions**, where they can be changed or forgotten. A blocked permission is refused without a question.
- Smooth scrolling, built in (**Settings > SCROLLING**, on by default). The mouse wheel and the keyboard (arrows, space, page up and down, home, end) scroll with an easing animation, like the Smooth Scroll extension. The distance of a wheel step (short, normal, long) and the smoothness (snappy, smooth, very smooth) can be chosen, and a change applies to open pages at once. A touchpad, a page that handles the wheel itself, text fields and media controls are left alone.
- Network speed in the status bar, bottom left: download (cyan) and upload (pink), updated every second. It is the speed of the whole computer over Ethernet, Wi-Fi and mobile data, not of NeonNet alone. A switch is in **Settings > LOOK** (on by default).
- Certificate warning page. A site with a bad certificate (expired, wrong name, unknown authority ...) now opens a clear NeonNet warning with **GO BACK TO SAFETY** as the default. Under **ADVANCED** the site can be opened anyway, until NeonNet is closed.
- HTTPS-only mode (**Settings > SECURITY**, off by default). An `http://` address is opened over `https://` instead. If the site has no working secure version, a warning asks before plain http is used, for that site until NeonNet is closed. Addresses on this computer, on the local network, and IP addresses are left alone.
- Block trackers and ads (**Settings > SECURITY**, or **Block trackers and ads** in the `≡` menu; off by default). Requests that a page makes to an advertising or tracking site are cancelled, using a list of about 82 000 domains built from EasyList, EasyPrivacy and Peter Lowe's ad server list (`tools/make_blocklist.py` rebuilds it). The page itself, and a site's own requests, are never blocked. The settings page shows how many requests were blocked.
- Saved passwords. After you log in, NeonNet asks whether to save the password (SAVE, NEVER FOR THIS SITE, NOT NOW); clicking into a login field later offers the saved user names, and the password is filled in only after you click one. Passwords are encrypted with Windows DPAPI in `passwords.dat`. `neonnet://passwords` lists and deletes them (a password cannot be shown). A switch in **Settings > PASSWORDS** turns the offer off; incognito windows never save or fill. Chromium's own password manager and autofill (and their unthemed "save" bubbles) are turned off.
- Page dialogs and permission prompts are shown in the NeonNet theme instead of Chromium's own windows: a page's **alert**, **confirm** and **prompt** (with a text box), the **leave this site?** question (also for reload), and permission requests for camera, microphone, screen sharing, location, notifications, clipboard and the like. A permission prompt lists what the site wants, and **Enter blocks** (the safe answer). The answer is remembered for that site (see **Site information and permissions** above), so the site is not asked again. A page that asks from a background tab is brought to the front first; several dialogs are shown one after the other.
- Site icons on tabs, with a switch in **Settings > START > Site icons on tabs** (on by default). The icon comes from the page's own icon address and is fetched like any other image; only http(s) addresses are used.
- Restore the last session. **Settings > START > Continue where you left off** brings back all tabs of the last session at start-up. With it off (the default) NeonNet starts with the home page, and pinned tabs still come back. Restored tabs start asleep and load when you select them, so many tabs do not load at once. The tabs are written to `%LOCALAPPDATA%\NeonNet\session.txt` by NeonNet itself, a few seconds after every change, so a crash loses little. An incognito window never saves or restores tabs. Reopening a closed pinned tab (Ctrl+Shift+T) pins it again.

## [1.5.0] - 2026-11-12

### Features
- Performance panel (Ctrl+Shift+M, the `PERF` button, or **Performance panel** in the `≡` menu): a side panel docked to the right of the page. It shows the memory NeonNet uses (pages, browser, engine) and CPU, and lists every tab with its own memory and CPU, marking tabs that use a lot of CPU as HOT.
  - **Clean memory:** every page collects its garbage, then unused memory is handed back to Windows.
  - **Sleeping tabs:** put a tab, or all background tabs, to sleep. A sleeping tab frees its memory, stays in the tab strip (dimmed) and reloads when you select it.
  - **RAM limiter** (off, or 1 to 8 GB): above the limit, the background tab unused the longest is put to sleep.
  - **CPU limiter:** tabs you are not looking at keep running, but with low priority and in Windows' power-saving mode.
  - **Protected tabs:** a tab is not put to sleep on its own while it plays sound, while a text field on it holds text you typed, or when its site is on your **kept awake** list (the lock on a tab). "Sleep background tabs" and the RAM limiter skip these tabs and say so.
- Sound indicator and mute: a tab that plays sound shows a speaker in the tab strip. Click it to mute the tab (the speaker is then crossed out); the panel has the same button. A muted tab stays muted when it sleeps and wakes.

## [1.4.0] - 2026-11-12

### Features
- Clear browsing data, in a new **PRIVACY** section of the settings page. Choose a period (last hour, 24 hours, 7 days, 4 weeks, all time) and whether to clear cookies and site data, the cache and the browsing history, in any combination. History entries are removed by the time of the visit. Cookies are removed by the time they were created. Site data (local storage, offline data) is cleared for the sites that lost a cookie, the sites in your history for that period, and the pages open now. The cache is always cleared completely. In an incognito window it clears the incognito session. The history page's **CLEAR ALL** button is replaced by a **CLEAR BROWSING DATA** link to this section (with only the history ticked), so there is one place to clear things.
- The right-click menu is drawn in the NeonNet theme (the same look as the `≡` menu, with a green frame in an incognito window) instead of the native Windows menu. It shows shortcuts, dims unavailable items, and works with the keyboard (arrows, Enter, Esc).
- New right-click items: **Open link in incognito window**, **Open image in new tab**, **View page source** (in a NeonNet tab) and, for selected text, **Search** with your chosen search engine. Chromium's own versions of these opened stray Chromium windows, and its Cast, QR code and "send to your devices" items, spell-check language submenus and settings links are no longer shown.
- Incognito window (Ctrl+Shift+N, or **New incognito window** in the `≡` menu). It is a separate window with a green frame and an INCOGNITO badge, and its own in-memory profile: cookies, cache, local storage and sign-ins are not written to disk and are gone when the window closes. No history is recorded, and bookmarks are shown but cannot be changed. Downloaded files are kept as usual. Several incognito tabs share one session, and the normal window is not affected. Automatic updates are handled by the normal window.

## [1.3.0] - 2026-11-12

### Features
- Videos NeonNet cannot play (H.264 / AAC, such as most `.mp4` and `.mov` files) can be opened in your own media player. A direct link to such a file asks whether to open it in the system player, save it, or do nothing; on a page, clicking play on a video that cannot be played opens it in the system player (also videos that only load when played, such as `preload="none"`; the built-in play button sends no click event to the page, so the play event is used), and right-clicking a video adds **Open in system player**. The player gets a temporary copy, which NeonNet deletes once the player has closed it. NeonNet does not include or install any player or codec.
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
