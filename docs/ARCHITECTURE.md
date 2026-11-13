# Architecture

How NeonNet works inside, for anyone changing the code.

## Overview

```
NeonNet.exe  (Chromium Embedded Framework bootstrap, renamed)
   │  loads NeonNet.dll and calls RunWinMain()
   ▼
NeonNet.dll  (src/main.cpp)
   ├─ browser process: main window, tabs, menus, status bar
   └─ sub-processes (renderer, GPU, network, ...): handed straight to CEF

Main window (Win32, painted with GDI)
   ├─ "chrome" browser  → ui/chrome.html  (tab strip + address bar)
   ├─ tab browser 1     → web page
   ├─ tab browser 2     → web page (hidden while not active)
   └─ status bar and frame
```

NeonNet is a plain Win32 application that embeds Chromium through the Chromium Embedded Framework (CEF). Every tab is a separate CEF browser, placed as a child window inside the main window.

## Why there is a `NeonNet.exe` and a `NeonNet.dll`

Since CEF 138, the Chromium sandbox on Windows can only be used through CEF's prebuilt `bootstrap.exe`, which is built with Chromium's own toolchain. As a result:

- NeonNet's code is built as a **DLL** that exports `RunWinMain(hInstance, cmdLine, nCmdShow, sandbox_info, version_info)`.
- `bootstrap.exe` is renamed to `NeonNet.exe`. When started, it loads `NeonNet.dll` from the same folder and calls `RunWinMain`.
- `tools/rescopy.cpp` replaces the icon and version info inside `NeonNet.exe` with those of `NeonNet.dll`, so Explorer and Task Manager show NeonNet.

`RunWinMain` is called by every process: the browser process and each sub-process. Its first call, `CefExecuteProcess`, returns an exit code straight away in sub-processes. Everything after it runs only in the browser process.

## Threads and message loop

NeonNet uses `CefRunMessageLoop()`: one UI thread runs both Windows messages and CEF work. This means:
- `WndProc` and all CEF UI-thread callbacks run on the same thread, so no locking is needed.
- The only code on another thread is `UiSchemeFactory::Create` (IO thread). It touches no state; it forwards commands to the UI thread with `PostMessageW(g.hwnd, WM_APP_CMD, ...)`.

## Main window

The window uses the `WS_OVERLAPPEDWINDOW` style, so snapping, the minimize animation and the taskbar behave normally. The standard Windows frame is removed:

| Message | Handling |
|---|---|
| `WM_NCCALCSIZE` | The whole window becomes client area. When maximized, the frame thickness is subtracted so the window does not spill off the screen. |
| `WM_NCHITTEST` | 5 px edges for resizing, the clipped corners for diagonal resizing, and the status bar as a drag area (`HTCAPTION`) |
| `SetWindowRgn` | Polygon shape with the top-right and bottom-left corners clipped by 16 px. Removed while maximized or fullscreen. |
| `WM_PAINT` | Background, the pink frame line following the clipped corners, and the status bar. GDI with double buffering. |

All sizes are written in DIPs (96 dpi) and converted with `S()` for the monitor's DPI. `WM_DPICHANGED` is handled.

Layout (`Layout()`):

```
┌──────────────────────────────── window ────────────────────────────────┐
│ 5px frame                                                              │
│ ┌──────────── ChromeRect: chrome browser (84 DIP) ────────────────┐    │
│ ├──────────── ContentRect: active tab browser ────────────────────┤    │
│ ├──────────── StatusRect: GDI status bar (24 DIP) ────────────────┤    │
│ └──────────────────────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────────────────────┘
```

Inactive tabs are hidden (`ShowWindow(SW_HIDE)` and `WasHidden(true)`) so Chromium can throttle them.

## The chrome UI and how it talks to the app

The tab strip and address bar are an ordinary HTML page (`ui/chrome.html`) in a CEF browser of its own. Both directions:

**UI → app:** the UI calls `fetch('https://neonnet.ui/cmd?m=<command>')`. The `m` value is the command name and its arguments separated by newlines, URL-encoded. The UI sends them one at a time (each request waits for the previous one), so the app receives them in order.

| Command | Arguments | Action |
|---|---|---|
| `ready` | | The UI has loaded; send the state |
| `navigate` | URL | Load the URL in the active tab (`javascript:` URLs are rejected) |
| `back`, `fwd`, `reload`, `stop` | | Navigation |
| `newtab` | | New tab |
| `select`, `close` | tab id | Select or close a tab |
| `scan` | | Toggle scanlines |
| `bookmark` | | Bookmark the active page, or remove its bookmark |
| `find` | text | Search the active tab |
| `findnext`, `findprev`, `findcase`, `findclose` | | Find bar buttons and keys |
| `menu` | x, y | Open the `≡` menu at the button |
| `min`, `max`, `quit` | | Window controls |
| `drag` | | Start moving the window (`WM_NCLBUTTONDOWN` + `HTCAPTION`) |
| `focusPage` | | Give focus back to the page |

**App → UI:** `SendState()` runs `window.__nn({...})` in the chrome browser with the full state: tabs (id, title, loading), active tab, URL, canBack, canFwd, loading, scan, max, canStar, starred, find, findCase, findText, search. The UI re-renders from that state. Two smaller messages exist: `findResult` (match count and selected match) and `focusFind`.

**Channel security:** `https://neonnet.ui/` is served by the app's own scheme handler, so these requests never reach the network. `/cmd` is only accepted from the chrome browser (checked by browser id). Web pages in tabs cannot send commands, even though they may load `https://neonnet.ui/newtab.html`.

## Embedded pages

| URL | Resource | Used by |
|---|---|---|
| `https://neonnet.ui/chrome.html` | `IDR_CHROME` | Chrome browser |
| `https://neonnet.ui/newtab.html` | `IDR_NEWTAB` | New tabs and the home button |
| `https://neonnet.ui/history.html` | `IDR_HISTORY` | History page (Ctrl+H) |
| `https://neonnet.ui/bookmarks.html` | `IDR_BOOKMARKS` | Bookmarks page (Ctrl+Shift+O) |
| `https://neonnet.ui/downloads.html` | `IDR_DOWNLOADS` | Downloads page (Ctrl+J) |
| `https://neonnet.ui/settings.html` | `IDR_SETTINGS` | Settings page (Ctrl+,) |
| `https://neonnet.ui/cmd?...` | | Command channel (answers 204) |
| `https://neonnet.ui/api/...` | | History and bookmarks data (JSON) |

Every response carries `X-Frame-Options: DENY`, so websites cannot load NeonNet's pages in a frame.

## History and bookmarks

`src/store.cpp` keeps both lists in memory and writes them to `%LOCALAPPDATA%\NeonNet\history.txt` and `bookmarks.txt`, one record per line: `unix-time-ms TAB url TAB title`, with tabs, newlines and backslashes escaped. Files are written to a `.tmp` file first and then renamed over the old one, two seconds after a change (`TIMER_SAVE`) and when NeonNet closes. An SRW lock protects the lists, because the UI thread records visits while the IO thread serves the API.

- **Recording:** `OnAddressChange` calls `store::Visit()` for http, https and file URLs (also for same-page navigations such as `pushState`). Reloading the newest entry updates its time instead of adding a row. `OnTitleChange` fills in the title. `OnLoadError` removes the visit again with `store::Forget()`.
- **Limits:** visits older than 90 days, and anything beyond 20,000 visits, are dropped when NeonNet starts.
- **API** (`https://neonnet.ui/api/`, served by `ServeApi()`):

  | Path | Answer |
  |---|---|
  | `history?q=` | Up to 1,500 visits, newest first, filtered by title or URL |
  | `history/delete?id=` | Remove one visit. Removing history by period is `cleardata/run` (see "Clear browsing data"); the history page links to it |
  | `bookmarks` | All bookmarks, oldest first |
  | `bookmarks/delete?id=`, `bookmarks/edit?id=&title=&url=` | Change bookmarks; `edit` rejects anything that is not http, https or file |
  | `downloads` | Up to 300 downloads, newest first (state, sizes, speed, `exists`, `openable`) |
  | `downloads/remove?id=`, `downloads/clear` | Remove one finished download from the list, or all finished ones |
  | `downloads/cancel?id=`, `downloads/open?id=`, `downloads/show?id=` | Posted to the UI thread as `WM_APP_DOWNLOAD` (see Downloads below) |
  | `settings` | Current settings (search engine, engine list and search URL, scanlines, automatic updates, installed copy, home page, ask-where-to-save, Downloads folder, versions) |
  | `settings/set?key=&value=` | Change one setting. Keys: `search` (an engine id), `scan`, `autoUpdate`, `askDownload` (`0` or `1`), `home` (empty, or an http, https or file address under 500 characters). Anything else answers 400 |

  The API answers only a **main frame that shows a `https://neonnet.ui/` page** in a tab (403 otherwise). A website's `fetch`, even with `no-cors`, cannot read or change the lists. Changes made through the API post `WM_APP_STORE` to the UI thread, which saves and refreshes the star.
- **Pages:** `history.html`, `bookmarks.html` and the new tab page fetch this API and build their lists with `textContent`. Links are only made for http, https and file URLs.

## Toolbar host and zoom

The toolbar (`chrome.html`) is served from `https://chrome.neonnet.ui/`, a second host handled by the same `UiSchemeFactory`, while the pages (new tab, history, bookmarks, downloads) stay on `https://neonnet.ui/`. Chromium stores zoom per host. On one shared host, zooming a page also zoomed the toolbar, which sits in a fixed-height area, so it shrank or was clipped. Its `/cmd` requests are relative, so they go to the same host. The `api/` calls still require a main frame on `https://neonnet.ui/`, so the toolbar host cannot use them.

## Downloads

`OnBeforeDownload` and `OnDownloadUpdated` in `Client` feed `store::DownloadUpdate()`, which creates the entry the first time it sees a CEF download id (so a download is listed as soon as the save dialog opens) and then updates sizes, speed and state. The list is written to `downloads.txt` (`time TAB url TAB name TAB path TAB total TAB state`) like the others, but only when a state changes (start, finish, cancel): saving on every progress report would keep restarting the 2-second save timer. Entries loaded from disk that were still running become `DL_INTERRUPTED`. At most 500 entries are kept.

`ServeApi()` runs on the IO thread, but cancelling needs the `CefDownloadItemCallback`, which `OnDownloadUpdated` keeps in `g.dlCallbacks` on the UI thread. So `downloads/cancel`, `open` and `show` only post `WM_APP_DOWNLOAD` with the entry id, and `DoDownloadAction()` does the work on the UI thread. The page never sends a path: the file is looked up by id in the list, must exist and be finished, and `OPEN` is refused for programs and scripts (`store::DownloadOpenable()`). `SHOW` runs `explorer.exe /select,"<path>"`. `downloads.html` polls `/api/downloads` once a second while it is visible and updates rows in place, so buttons stay clickable.

`OnContextInitialized` also sets the Chromium preference `download_bubble.partial_view_enabled` to false (the "Don't show when downloads finish" option). Without it Chromium pops up its own unthemed download bubble after every download. A `disable-features=DownloadBubble` switch did not remove it; the preference does.

## Performance panel

The panel is a second UI browser, `g.panel`, showing `performance.html` on `chrome.neonnet.ui` (like the toolbar: its own zoom, and allowed to use `/cmd`; `api/` stays closed to it). It is created when opened and closed when hidden, so it costs nothing while closed. `ContentRect()` loses `PANEL_DIP` on the right while it is open, `PanelRect()` is the strip it takes, and `Layout()` places it. `IsUiBrowser()` (toolbar or panel) replaces `IsChrome()` where the rule is "NeonNet's own UI": no popups, no context menu. The app pushes data to the page with `ExecuteJavaScript("nnPerf(json)")` every 2 seconds (`TIMER_PERF`; 4 seconds when only a limiter is on, the timer stops when nothing needs it), and the page sends commands as `perf.*` through `/cmd` (`OnPerfCommand()`).

**Tab to process:** a renderer sends its process id to the browser process from `OnContextCreated` for every main frame (`nn.pid`), which sets `Tab::pid`. CEF has no call for it, and a DevTools or task manager route does not map tabs to processes. Tabs that share a process (same site, related windows) get the same figures, flagged `shared`. Cross-process iframes use other processes and are not counted for the tab.

`perf.cpp` has the Windows part: `Sample()` takes one process snapshot, follows the parent ids from this process, and reads each one's private working set (`PROCESS_MEMORY_COUNTERS_EX2`, declared in `perf.cpp` because the header target is older) and CPU time, as a share of all processors since the last sample. `TrimWorkingSets()` calls `EmptyWorkingSet` on all of them, and `SetBackground()` sets `BELOW_NORMAL_PRIORITY_CLASS` plus the power-throttling state (EcoQoS). Clean memory (`CleanMemory()`) sends `HeapProfiler.collectGarbage` to every tab, and 1.2 seconds later (`TIMER_TRIM`) trims all working sets and samples again. Trimming changes the working set, not the commit: pages come back when used, so the drop is not a lasting saving.

**Sleeping tabs:** `UnloadTab()` sets `Tab::discarding` and calls `CloseBrowser(true)`; `OnBeforeClose()` then keeps the tab, sets `discarded` and clears the browser, instead of `CloseTab()`. `SelectTab()` calls `RestoreTab()`, which creates a new browser for the tab's address, like `CreateTab()`. The selected tab is never unloaded, and closing is forced, so a page cannot ask "leave this page?". The RAM limiter (`ApplyRamLimit()`) picks the non-active, non-loading tab with the oldest `lastActive` every time `TIMER_PERF` fires while the total is over the limit, at most one per 5 seconds so the figures can catch up. The CPU limiter (`ApplyBackgroundLimit()`) lowers the priority of the renderer of every tab except the selected one's, and remembers what it set per process. Both settings are in the registry (`RamLimitMB`, `BgLimit`).

## Smooth scrolling

`SMOOTH_JS` is evaluated in the main frame of every http(s) page next to the other hooks. It listens for `wheel` and `keydown` on the window in the bubbling phase (so the page's own handlers run first and a handled event is left alone, `defaultPrevented`), finds the element that can scroll in that direction (`targetFor()`, falling back to the document), and animates it with `requestAnimationFrame` and `scrollTo({behavior: 'instant'})` (so a page's CSS `scroll-behavior` does not smooth twice). A new wheel step extends the target of a running animation and restarts the easing from the current position. Wheel events with small or fractional `deltaY` (precision touchpads) are not touched.

The script learns its settings from the browser process: the renderer's `nn.pid` message (sent for every new page) is answered with `nn.cfg` (on, step, time), and `ApplySetting()` sends `nn.cfg` to the main frame of every tab when a smooth scrolling setting changes. The renderer keeps the script's receiving function in C++ (`g_cfgRecv`, by frame; registered through `__nnpc`, which the script removes from the global object), as for the password manager. Registry values: `SmoothScroll`, `SmoothStep` (70, 120 or 200: 120 means a 100 px wheel step moves 120 px) and `SmoothTime` (250, 400 or 700 ms).

## Docked developer tools

`OpenDevTools()` calls `ShowDevTools()` with `SetAsChild(g.hwnd, ContentRect())`: the tools are a child window of the NeonNet window (or, for the "separate window" position, a popup). `Tab::dev` holds their browser (set in `DevToolsClient::OnAfterCreated`, cleared in `OnBeforeClose`); F12 closes them when open (`CloseDevTools()`).

Docked tools work the way they do in Chrome: they are started with `can_dock=true` (`OnLoadEnd` loads their address once more with it, since an embedded window does not get it; that parameter is also what shows the device toolbar and the Dock side row of their menu) and then lay themselves out over the whole page area, bottom, right or left, leaving a hole where the page belongs. They report that hole with `setInspectedPageBounds`; `DEVTOOLS_DOCK_JS` (run in the tools page) wraps `DevToolsHost.sendMessageToEmbedder` and passes it on as a console message (`__NN_BOUNDS__x,y,w,h`) that `DevToolsClient::OnConsoleMessage` stores in `Tab::devHole` (scaled to pixels). `Layout()` puts the tools' window under everything (`HWND_BOTTOM`, full area) and the page's window into the hole. So a dock side change, the splitter and the device toolbar (which draws its frame inside the hole) need nothing else from NeonNet.

The same wrapper reports a dock side choice (`setPreference currentDockState`, as `__NN_DOCK__<side>`), which is saved in the registry (`DevToolsDock`: 0 bottom, 1 right, 2 left, 3 separate window; **Settings > DEVELOPER TOOLS** sets the same value). Changing between docked and a window of its own closes the tools and opens them again (`WM_APP_DEVOPEN`), because the kind of host window differs. The tools remember their own last side and start out unaware of NeonNet's choice, so after they load the script imports their `DockController` and sets the wanted side, then says `__NN_READY__`; dock messages before that are ignored. This depends on the tools frontend's module path and API of this Chromium version; if they change, the position may not follow the setting (the page then still shows fully).

## Site permissions and the lock panel

`PERM_DEFS` lists the permissions NeonNet knows (key, label, the CEF permission and media flags that mean it). `PermKeys()` turns a request (`OnShowPermissionPrompt` or `OnRequestMediaAccessPermission`) into keys. `ShowDlgReq()` first looks each key up in the store (`g_sp`, origin -> key -> 1 allow / 2 block, guarded by `g_spMu`): any block denies, all allowed accepts, otherwise the themed dialog asks and the answer is stored for every key of the request. The store is `sitepermissions.txt` next to the password file (a plain list: origin, key, value; loaded by `SpLoad()`, not read or written in an incognito window).

`SendState()` adds `site` (`SiteJson()`: origin, host, kind secure/insecure/internal, `warn` for a certificate exception, and the permission values) to the state the toolbar gets. The lock panel in `chrome.html` is drawn like the address suggestions (the host makes the toolbar taller through `dropdown`, and clips the toolbar window to its top row plus the panel's x range so the page stays visible beside it). Its buttons send `siteperm\n<key>\n<0|1|2>`; the host applies it to the origin of the page that is shown, never to an address the toolbar names. `opentab` opens a NeonNet page in a new tab. `neonnet://permissions` (`ui/permissions.html`) uses `/api/siteperms`, `/set`, `/reset` and `/clear`, validated with `SpOriginValid()` and `PermKeyValid()`.

## Network speed

`NetSample()` (every second, `TIMER_NET`) reads `GetIfTable()` and, per adapter of type Ethernet, IEEE 802.11 or mobile broadband that is operational and whose description does not name a virtual, VPN, TAP, Bluetooth or WAN miniport device, takes the difference of the 32-bit in and out octet counters since the last sample (unsigned subtraction, so a counter wrap is harmless) and divides it by the elapsed time. `Paint()` draws both rates in two fixed-width columns at the left of the status bar, so the status text after them does not jump. The timer stops when the setting is off, and a minimized window is not sampled. It measures the whole computer: per-process traffic is not available from the adapter counters.

## Certificate errors, HTTPS-only mode and the warning page

`OnCertificateError()` returns `false` (the load is cancelled) unless the user chose to proceed for that `host:port` earlier in this run (`g.certAllow`, then `callback->Continue()`). The cancelled navigation ends in `OnLoadError()`, which for a certificate code (-200 to -219) loads `https://neonnet.ui/warning.html?kind=cert&code=...&url=...` and shows the failed address in the address bar (`errorUrl`, as for the SIGNAL LOST page). The warning page is a NeonNet page, so its **PROCEED** button can call `/api/settings/set?key=proceed&value=cert|<url>`; `ApplySetting()` honours it only for an address NeonNet itself warned about (`g.warned`), so a web page that links to `warning.html` cannot create an exception. Exceptions live in memory only.

HTTPS-only mode (`g.httpsOnly`, registry `HttpsOnly`): `OnBeforeBrowse()` cancels a main frame `GET` to `http://` (unless `HttpsExempt()`: no dot, an IP address, `.local`, `.localhost` ...; or the host is in `g.httpAllow`) and posts a `NavTask` that loads the `https://` address. `Tab::upgradedFrom/upgradedTo` remember both: when the `https://` load fails (`OnLoadError()`), the warning page `kind=https` is shown for the original address, and **CONTINUE** adds the host to `g.httpAllow`.

## Tracker and ad blocking

`ui/blocklist.txt` (a resource, `IDR_BLOCKLIST`) is generated by `tools/make_blocklist.py`: the `||domain^` rules (without path, and with no options or only third-party and resource-type options) of EasyList and EasyPrivacy, Peter Lowe's list, and `tools/blocklist-extra.txt`, with subdomains of listed domains dropped. `LoadBlocklist()` reads it into an `unordered_set` the first time blocking is on and then sets `g_blocklistReady`; the set is never changed after that, so the IO thread reads it without a lock. `Client::GetResourceRequestHandler()` (IO thread) returns a handler whose `OnBeforeResourceLoad()` returns `RV_CANCEL` only when blocking is on, the request is not a navigation, its host or a parent domain is listed, and the page (main frame address) is not the same site (`SiteOf()`: the last two labels, three for `co.uk`-style endings). `g.blockedCount` is shown on the settings page. NeonNet's own pages and the toolbar are never filtered.

## Saved passwords

`passwords.dat` holds one line per login (`E`, an encrypted blob with the site and user name, and an encrypted blob with the password) and one per never-save site (`N`); every blob is `CryptProtectData` output with a fixed entropy string, base64 encoded. In memory the password stays encrypted (`PwEntry::passBlob`) and is decrypted for the moment of filling; the list is guarded by `g_pwMu` because the manager page reads it on the IO thread. Chromium's own password manager and autofill are turned off with preferences (`credentials_enable_service`, `profile.password_manager_enabled`, `autofill.*`) in both request contexts.

The page side is `PW_HOOK_JS`, evaluated in the main frame of every http(s) page in `OnContextCreated()` next to `MEDIA_HOOK_JS`. It receives two native functions (`send` and `reg`, removed from the global object at once, like the media reporter). It reports a login being sent (a trusted `submit`, click on a button, or Enter in a password field: `isTrusted` is checked), then whether the password field went away on the same page (`ok`) or what the next page shows (`page`, sent by every page). It shows the list of saved user names in a closed shadow root under the field, and fills the fields only when a trusted click on an entry comes back as a `fill` message. The browser process (`Client::OnPw()`) keeps what was typed in `Tab::pw` for 25 seconds, asks only when the login seems to have worked, and works from the frame's address as the browser knows it, never from what the page claims. Replies go to the renderer as the process message `nn.pw`; the renderer keeps the page's receiving function in C++ (`g_pwRecv`, by frame), out of the page's reach. The save question is a `DlgReq::PWSAVE` in the normal dialog queue.

## Dialogs and permission prompts

`Client` is also a `CefJSDialogHandler` and a `CefPermissionHandler`. `OnJSDialog()` (alert, confirm, prompt), `OnBeforeUnloadDialog()`, `OnShowPermissionPrompt()` and `OnRequestMediaAccessPermission()` do not show anything: they put a `DlgReq` (kind, origin, text, and the CEF callback) in a posted `WM_APP_DIALOG` message and return `true`. The window procedure runs `OnDialogMsg()`, which queues the request and shows them one at a time with `ShowDlgReq()`; it then answers the callback (so a callback is never answered from inside the browser's own callback, the same pattern as the right-click menu). `RunDialog()` builds one window class (`NeonNetDialog`): title, message, optional `EDIT` controls and clipped buttons, painted like the menus (`PaintScanlines()` is shared with `PaintMenu()`). It disables the main window (`EnableWindow(FALSE)`) while it runs its own message loop, handles Enter, Esc and Tab itself, and answers `cancelId` when NeonNet closes, so no callback is left open. `g.permMemo` keeps allow/block answers by origin and permission set for the life of the process. Pages of NeonNet itself (`IsUiBrowser()`) never get a prompt: they are answered with "deny", which also stops Chromium's "access other apps on this device" bubble when the toolbar loads a site icon from a local address.

HTTP sign-in is not covered (so the password manager does not cover it either): in this Chrome-style build `CefRequestHandler::GetAuthCredentials()` is never called (a trace in the handler stayed empty), and the request ends with `ERR_INVALID_AUTH_CREDENTIALS` (-338), shown as the SIGNAL LOST page.

## Tab order, pinned tabs and session restore

**Order:** `g.tabs` is the strip, left to right. `Tab::pinned` tabs always form the left end: `KeepPinsFirst()` (a `stable_partition`) runs after every insert (`CreateTab`, popups), `SetPinned()` puts a tab at the end of the pinned group or first after it, and `MoveTab()` clamps the target into the tab's own group. `chrome.html` sends `move`, `pin` and `tabmenu` commands (arguments separated by newlines: the tab id, then the new index, or the click position and `devicePixelRatio`); `ShowTabMenu()` opens the themed `RunMenu()` at the click and runs the choice itself (`T_PIN`, `T_DUP`, ...). `SendState()` carries `pinned` for each tab.

**Drag:** done in JavaScript with mouse events, not HTML5 drag and drop. A press selects the tab at once and records `drag`; after 5 px of movement the tab is lifted: it gets a `translateX` so it follows the pointer, and `insertBefore()` swaps it with a neighbour whenever its centre passes the neighbour's centre (only within the same pinned/unpinned group). `render()` returns early while a tab is dragged (a state push would rebuild the strip and lose the element), and the tab is found again by `data-id`, because the `select` answer of the press may already have rebuilt it. On release the final index is sent with `move`; the host answers with `SendState()` even when nothing moved, so the strip snaps back to the real order. A `mousemove` with no button down, or a window blur, ends a drag whose release was missed.

**Site icons:** `OnFaviconURLChange()` keeps the first `http(s)` icon address (under 2000 characters) in `Tab::icon`; `OnAddressChange()` clears it when the site changes. `SendState()` adds it to the tab as `icon` when `g.favicons` (registry `ShowFavicons`, default on) is set, and `chrome.html` loads it with an `img` (hidden again on error). The host never downloads the icon itself.

**Session file:** `%LOCALAPPDATA%\NeonNet\session.txt`, written by NeonNet itself because Chromium's restore needs `persist_session_cookies`, which stays off (see the table below). First line `NEONNET-SESSION 1`, then one line per tab: flags (`P` pinned, `A` active, `-`), title, address, separated by tabs. Only `http://`, `https://` and `file:///` addresses are saved, at most 200 tabs; a tab on an error page saves the address it failed to load (`DisplayUrl()`). `SendState()` calls `ScheduleSession()`, which starts `TIMER_SESSION` (3 s) unless it is already running, so the timer is not pushed back by a stream of changes; `SaveSession()` skips the write when the text is unchanged, and writes to `session.txt.tmp` and swaps it in with `MoveFileEx`. `BeginClose()` calls `SaveSession()` before anything is closed, because `OnBeforeClose()` removes tabs. After `g.closing` nothing is written, and an incognito process never reads or writes the file.

**Restore:** `OpenStartTabs()` (from `OnContextInitialized()`) reads the file. Without the registry value `RestoreSession` only the pinned tabs are kept, and the start address or home page is opened as a new tab after them; with it, all tabs are kept and the start address, if there is one, is added as a new tab. A lone empty new tab is not restored (the home page is better). Restored tabs are `Tab`s with `discarded = true` and no browser, the same state as a tab put to sleep, so only the selected one gets a browser, through `SelectTab()` and `RestoreTab()`. Sleeping tabs show their saved title until they load.

## Sound, mute and sleep protection

**Mute:** `CefBrowserHost::SetAudioMuted()`, with `Tab::muted` kept in the tab, so it is applied again when the browser is recreated (`OnAfterCreated`). Checked against the Windows audio session meter: a tone played by a tab drops from 0.2 to 0 on mute and comes back on unmute.

**Detecting sound:** `CefAudioHandler` is never called in this Chrome-style build (a handler on the client got no callback at all, not even `GetAudioParameters`), and there is no audible-state callback, so the renderer detects it. `MEDIA_HOOK_JS` arms a 1-second timer when a media element fires `play` or `playing`. Each tick looks at the `video` and `audio` elements: playing, not ended, not muted, volume above zero, `readyState` of at least 3, and `webkitAudioDecodedByteCount` has grown since the last tick (so a video without an audio track does not count). If any qualifies it sends `r('audio','1')` (the process message `nn.media`), and the browser process sets `Tab::audioUntil` 3.5 seconds ahead. `OnAudioTimer()` (`TIMER_AUDIO`, 1 second) turns that into `Tab::audible`, which is false for a muted tab, and calls `SendState()` when it changes; `chrome.html` draws the speaker (`.snd` / `.muted`). Web Audio sound alone is not detected: it would need a patch of `AudioNode.prototype.connect` on every page, which was left out on purpose.

**Typed text:** the same hook watches `input` and `change` (capture phase) and, 500 ms after the last one, scans up to 300 `textarea`, `input` (not hidden, checkbox, radio, button, submit, search, file ...) and editable elements for a value with a non-space character. It reports changes only (`r('dirty','1'|'0')`) and `OnLoadStart` clears `Tab::dirty` for a new page.

**Protection:** `SleepProtection()` returns `audio` (`audioUntil` still in the future, muted or not), `form` or `kept` (`HostOf()`, the host without `www.`, is in `g.keepHosts`, stored in the registry value `KeepAwakeHosts`, separated by `;`). `ApplyRamLimit()` and `perf.unloadbg` skip such tabs (`g.sleepBlocked` counts them for the panel); the per-tab moon button does not check, because the user chose it. The panel's lock sends `perf.keep`, and `✕` in the list sends `perf.unkeep` with the host.

## Clear browsing data

`settings.html` calls `api/cleardata/run?range=<hour|day|week|month|all>&cookies=0|1&cache=0|1` and polls `api/cleardata/status` (`g.clearState`: 0 idle, 1 running, 2 done, plus counts). The page sends a range name, never a time: `ServeApi()` turns it into a cut-off and posts `WM_APP_CLEARDATA` to the UI thread, where `StartClearData()` runs. CEF has no "delete everything since" call, so the work is split:

- **Cookies:** `CefCookieManager::VisitAllCookies()` with `CookieClearer`, which sets `deleteCookie` for cookies whose `creation` (microseconds since 1601) is at or after the cut-off, and remembers their domains. The context is `g.reqCtx` in an incognito window, the global one otherwise.
- **Site data:** the visitor's destructor (it runs when the visit is over, also when there were no cookies) posts `WM_APP_CLEARSITES`, and `ClearSiteData()` sends `Storage.clearDataForOrigin` through `ExecuteDevToolsMethod()` for each origin: both schemes of the cookie domains, `store::OriginsSince()` (history in the period), and the pages open now (cookies carry no port, and an incognito window has no history). Storage types are everything except cookies. This is a best match, not a full sweep: the engine offers no list of sites with stored data. At most 600 origins.
- **History:** `store::ClearHistorySince()` removes the entries whose visit time is at or after the cut-off (all of them for 0), and `ClearHistoryData()` schedules the save and refreshes the toolbar state. With cookies selected it runs in `ClearSiteData()`, after `OriginsSince()` has read the same history, so the site data of the visited sites is still found. `api/cleardata/status` also reports `history`, the number of entries removed.
- **Cache:** `Network.clearBrowserCache` through `ExecuteDevToolsMethod()`, always the whole cache. `CefRequestContext::ClearHttpCache()` crashes libcef in this build (access violation at the same offset, in the normal profile and in an incognito one), so it is not used. Checked by fetching an asset with `cache: 'force-cache'` before and after: it is served from the cache before, and from the network after.

A DevTools command sent this way, followed by `Browser.close` from an external debugging client, crashed the process once on exit during testing; closing the window normally did not. The DevTools command has no observer registered, so nothing waits for its result.

## Right-click menu

`Client::RunContextMenu()` takes over the menu (CEF calls it for Chrome-style browsers). It copies the `CefMenuModel` into `MenuItem` rows right away (`AppendMenuModel()`; the model must not be kept), posts `WM_APP_CTXMENU`, and `ShowContextMenu()` runs `RunMenu()`, the same painted menu window as the `≡` menu, with the title `CONTEXT` and the menu's top-left corner at the cursor (`anchorLeft`). The position is the cursor position when the callback ran, not `params->GetXCoord()`, whose scale differs. The result goes to `callback->Continue()`, or `Cancel()` for Esc, so Chromium runs its own commands and `OnContextMenuCommand` gets ours. An empty model (the toolbar outside a text box) is cancelled at once.

Chromium's command ids in this build are large numbers, not below `MENU_ID_USER_FIRST`, so NeonNet's items are told apart by their own id range (`MENU_OPEN_LINK_TAB` to `MENU_SEARCH_SELECTION`). Chromium's commands that open a new tab or window do not use NeonNet's tabs: choosing View page source created a separate Chromium window with the omnibox popup. `ReplacedByNeonNet()` therefore hides them by label (open link or image in new tab, new window, incognito, view page source, inspect, search, Cast, QR code, send to your devices, language settings, copy link to highlight), and `OnBeforeContextMenu()` adds NeonNet's versions: they call `CreateTab()`, `LaunchIncognito(url)` or `ShowDevTools()`. Submenus (spell check, writing direction) are not shown. Commands that act inside the page (back, reload, save as, print, copy, paste, select all, spell-check suggestions) are left to Chromium and work through `Continue()`.

## Incognito window

An incognito window is a second NeonNet process started with `--neonnet-incognito` (`LaunchIncognito()`, run by `CMD_INCOGNITO`, Ctrl+Shift+N). `g` is a single global, so a process-per-window design avoids turning every function into one that takes a window.

Isolation has two layers. First, the process has its own `root_cache_path` (`%LOCALAPPDATA%\NeonNet\Incognito`), so it does not hold the normal profile's single-instance lock, and the engine's own files for it can be deleted. Second, and this is what keeps site data off the disk, `OnContextInitialized` creates a `CefRequestContext` with default settings (an empty `cache_path`, which Chromium treats as incognito: cookies, cache, local storage and the rest stay in memory) and `g.reqCtx` is passed to every `CreateBrowserSync` for tabs. Scheme handler factories and preferences are per request context, so `neonnet.ui`, `chrome.neonnet.ui` and the download bubble preference are registered on it as well. The toolbar itself stays on the global context; it stores nothing.

A `Local\NeonNetIncognitoSession` mutex makes the first incognito process the owner. A second one finds the mutex, fails `CefInitialize` on the same `root_cache_path`, and CEF delivers its arguments to the first through `OnAlreadyRunningAppRelaunch`: one incognito session with several tabs, like Chrome's single incognito window. The owner deletes a stale session folder at start (a crash or `Browser.close` leaves one). On a normal close it starts `StartIncognitoCleanup()`, because `CefShutdown()` can end the process without returning: a detached copy of NeonNet (`--neonnet-purge-incognito=<pid>`, handled at the top of `RunMain` before CEF starts) waits for the process to exit and deletes the folder.

In the process, `g.incognito` changes these things: `store::LoadBookmarksReadOnly()` reads `bookmarks.txt` but clears the file names, so `SaveIfDirty()` writes nothing; `Visit()`, `AddBookmark()`, `EditBookmark()` and `DeleteBookmark()` do nothing, so no history and no bookmark changes (downloads stay in memory only); window placement is neither restored nor saved; updates and the start-up update report are skipped; the temporary player copies use `Temp\media-incognito`, so the normal window's sweep never touches them; and the process sets its own AppUserModelID so Windows gives it a separate taskbar button. The frame, menu marks and status bar line use `Accent()` (green), the toolbar and new tab page switch the `--pink` variable to green through `state.incognito` and `api/settings`, and the new tab page shows a short notice, including that websites and the network can still see the visits.

## Unplayable media and DRM

The bundled CEF has no proprietary codecs (checked with `canPlayType` and `MediaSource.isTypeSupported`: H.264, AAC and HEVC are refused, VP9, AV1, Opus and MP3 work, and `requestMediaKeySystemAccess` rejects Widevine). Enabling them means building CEF with `proprietary_codecs=true ffmpeg_branding=Chrome`, which brings patent licensing duties; the OS-decoder-only variant (CEF issue 3559) gives H.264 video without AAC audio, so it does not help. NeonNet instead hands such files to the user's own player and does not distribute a decoder.

`NeonApp` is also a `CefRenderProcessHandler` (it is passed to `CefExecuteProcess`, so renderer processes get it). `OnContextCreated` puts a native function `__nn` on the global object of every http(s) page (not the NeonNet pages) and evaluates `MEDIA_HOOK_JS`, which takes the function and deletes the global before any page script runs, so a page cannot call it. The script wraps `navigator.requestMediaKeySystemAccess` (a rejected Widevine, PlayReady or FairPlay request reports `drm`) and listens, in the capture phase, for `error` events: a `<video>` or `<audio>` with error code 4, or the last `<source>` of an element whose `networkState` is 3 (`media`; the element itself has no error object in that case). When the whole page is that one element (a direct link to a file, shown by Chromium as a media document) it reports `mediadoc` with the content type. Each kind is sent once per page load as the process message `nn.media`. `Client::OnProcessMessageReceived` sets `g.alert`, which the status bar paints in pink above everything else for 12 seconds (`TIMER_ALERT`); `DRM` wins over `media`, because it explains the failure. Alerts are shown for the active tab only.

For `mediadoc`, and for a download of a type in `IsUnplayableMime()` that is not an attachment (`OnBeforeDownload`), a `MediaAsk` is posted as `WM_APP_MEDIA` and `AskMedia()` shows a Yes/No/Cancel box from the window procedure, not from inside the CEF callback. Chromium does not turn a direct `video/mp4` link into a download in this build; it opens the media document, which is why the renderer reports it. For a media document the answer is stored as `g.pendingPlayerUrl` or `g.pendingSaveUrl` and `StartDownload()` is called; `OnBeforeDownload` recognises the URL and skips the question. The context menu item (`MENU_OPEN_PLAYER`, shown for video and audio with an http(s) source) uses the same `pendingPlayerUrl` path. So does a click on a failed `<video>`/`<audio>` (a capture-phase `click` listener in the hook reports `openplayer` with the source address; the browser process accepts only http(s), ignores it while a copy is being prepared or within 5 seconds of the last one, and then starts the download).

A copy for the player goes to `%LOCALAPPDATA%\NeonNet\Temp\media` (`TempMediaPath()`; the extension is checked against a list of media extensions or taken from the MIME type, so a server cannot make this start a program). `g.tempDl` maps the CEF download id to the file, so `OnDownloadUpdated` keeps these out of the downloads list; on completion `OpenTempMedia()` calls `ShellExecuteW` and records the time in `g.tempOpened`. There is no reliable way to know when a player is finished (many hand the file to an already running instance, so the process started here exits at once), so `SweepTemp()` deletes instead: every 20 seconds while the folder has files (`TIMER_TEMP`), at start-up and in `BeginClose()`. A file is deleted only when `DeleteFileW` succeeds, which Windows refuses while a player keeps it open, and not within two minutes of being opened (a player that has not opened it yet, or that reads it without a lock). Nothing is swept while a copy is still downloading. If `ShellExecuteW` finds no program, the file is moved to Downloads.

## Settings

Values live in the registry (`HKCU\Software\NeonNet`): `SearchEngine` (string, an id from `SEARCH_ENGINES`), `Scanlines` and `AutoUpdate` (DWORD), `HomePage` (string, absent = new tab page) and `AskDownload` (DWORD, absent = 1). `ServeApi()` runs on the IO thread, so `api/settings` reads only atomics (`g.engine`, `g.scan`) and the registry. `api/settings/set` validates the key and value there, then applies the change with `SendMessageTimeout(WM_APP_SETTING)` to the UI thread and waits (at most 2 seconds), so the page can read the new value straight back. `ApplySetting()` on the UI thread writes the registry and refreshes what depends on it: a changed engine goes to the toolbar in the state message (`search`, the URL prefix that `chrome.html` appends the encoded query to), scanlines reuse `CMD_SCAN`. `newtab.html` reads `searchUrl` from `api/settings`. `HomeUrl()` is used by the home button (`CMD_HOME`) and for the first tab. With `AskDownload` off, `OnBeforeDownload` calls `Continue(path, false)` with a path from `DownloadTarget()` in the Downloads folder (name sanitised, `" (n)"` added until the file is new).

## Short addresses

NeonNet's pages are really `https://neonnet.ui/<name>.html`. `chrome.html` shows them as `neonnet://<name>` (`shownUrl()`) and turns a typed `neonnet://<name>` back into the real address (`toUrl()`); `NormalizeArgUrl()` does the same for a command-line argument. This is only a display and input alias: the page's origin, the history and the `api/` checks still use the real `https://neonnet.ui/` address. A real `neonnet://` scheme would have to be registered in every Chromium process and would give each page its own host, and so its own zoom.

## Address bar suggestions

The toolbar fetches `/suggest?q=` from its own host (`chrome.neonnet.ui`); the factory answers only the toolbar browser (403 for any other frame), and the text never leaves the process. `store::SuggestJson()` returns up to 8 entries: bookmarks and history entries that contain every word of the query (case-insensitive, in the address or title), merged by address, scored by: address starts with the typed text (+60), else host starts with the first word (+45) or contains it (+25); a title word starts with a typed word (+8); bookmark (+25); visits (1.2 each, up to 25); recency (up to +20, decaying over about 10 days). `chrome.html` debounces input by 40 ms, drops answers that arrive out of order, and builds the list with `textContent`.

The list needs room over the page, so the toolbar browser grows: `chrome.html` measures the list and sends `dropdown <height in CSS px>` (0 when closed). `ChromeHostRect()` makes the toolbar window that tall and `Place()` raises it above the page windows. `ContentRect()` does not change, so the page below is not re-laid out. The window width does not change; a width change closes the list (a height change is expected, because the host causes it). Arrow keys, Enter and Esc are handled in the page itself.

## Find in page

## Find in page

Ctrl+F sets `g.findOpen`, which makes `ChromeRect()` `FIND_DIP` (34 DIP) taller, so the chrome browser shows the find row of `chrome.html` and the tabs move down. Each search calls `CefBrowserHost::Find(text, forward, matchCase, findNext=true)`. With `findNext=false` Chromium only highlights the matches without selecting one, and the first "next" would then select match 1 again. `OnFindResult` sends the count to the UI. Switching tabs, opening a tab or a popup closes the bar and calls `StopFinding(true)` on the old tab (`HideFind()`).

## Reopening closed tabs

`CloseTab()` pushes the tab's URL and strip position onto `g.closed` (at most 25; not while the window is closing, and never for the new tab page). Ctrl+Shift+T pops the newest one and creates the tab at the same position. Only the URL comes back, not the tab's back/forward history: CEF cannot move a navigation history between browsers.

The HTML files are embedded as `RCDATA` in `NeonNet.dll`, so there are no loose UI files that could be changed from outside.

## Tab lifecycle

| Event | What happens |
|---|---|
| New tab | `CreateTab()` adds a `Tab` entry, then calls `CefBrowserHost::CreateBrowserSync` with `SetAsChild(main window, ContentRect)`. `OnAfterCreated` matches the browser to the tab through `g.creatingTab`. |
| Popup (`window.open`, `target=_blank`) | `OnBeforePopup` adds a `Tab` entry and allows the popup as a child window in the content area. `window.opener` keeps working, so OAuth sign-ins work. The popup browser is matched through the `g.pendingPopups` queue. |
| Title, URL, loading, back/forward | `OnTitleChange`, `OnAddressChange`, `OnLoadingStateChange` → `SendState()` |
| Load failure | `OnLoadError` (except `ERR_ABORTED`) loads the "SIGNAL LOST" page as a `data:` URL. The failed URL is kept in `errorUrl`, so the address bar keeps showing it and reload retries it. |
| Renderer crash | `OnRenderProcessTerminated` shows the same page |
| Closing a tab | `CloseTab()` hides the browser, then calls `CloseBrowser(false)`, so the page's `onbeforeunload` still runs |
| The page calls `window.close()` | `OnBeforeClose` removes the tab |
| The last tab is closed | A new tab is created right away, so the window is never empty |

## Other features

- **Scanlines:** `ScanScript()` inserts one `position:fixed` `div` with `pointer-events:none` and a line gradient. It runs on `OnLoadStart` and when loading finishes. The script is idempotent: it never stacks overlays, and removes the overlay when scanlines are off.
- **`≡` menu:** a native menu cannot be recolored, so `RunMenu()` shows a `WS_POPUP` window (class `NeonNetMenu`) painted with GDI: clipped-corner region, pink outline, cyan highlight and scanlines drawn with `GdiAlphaBlend`. It runs its own message loop until an item is chosen, Esc is pressed or the popup loses activation, and returns the command like `TrackPopupMenu` does. A `menu` request that arrives within 250 ms of the menu closing is ignored, so a second click on `≡` closes the menu instead of reopening it.
- **Shortcuts:** `OnPreKeyEvent` catches keys before the page does, for every browser (chrome and tabs).
- **Fullscreen:** combines two sources: `fsUser` from F11, and `fsElement` from the page (for example a video) through `OnFullscreenModeChange`. In fullscreen the window covers the whole monitor and the chrome and status bar are hidden.
- **Downloads:** `OnBeforeDownload` opens a save dialog. `OnDownloadUpdated` shows progress in the status bar.
- **Single instance:** CEF locks the profile folder. A second instance fails in `CefInitialize`, and Chromium forwards its command line to the first instance through `OnAlreadyRunningAppRelaunch`, which opens the URL in a new tab.

## Chromium settings and why

| Setting | Value | Reason |
|---|---|---|
| `root_cache_path` / `cache_path` | `%LOCALAPPDATA%\NeonNet\Profile` | Persistent per-user profile |
| `persist_session_cookies` | `false` | When `true`, CEF switches Chromium to "restore last session", and old tabs come back as plain Chromium windows outside NeonNet |
| `session.restore_on_startup` preference | `5` (new tab) | Extra safeguard for the same problem |
| `hide-crash-restore-bubble` switch | on | No "Restore pages?" bubble |
| `disable-extensions` switch | on | Other software (for example download managers) registers Chrome extensions system-wide. Chromium would load them and open their pages as stray Chrome windows. Built-in components such as the PDF viewer keep working. |
| `background_color` | `#12062c` | No white flash while pages load |
| Browser style | Alloy (automatic with `SetAsChild`) | A browser without Chrome's own UI, so all UI is NeonNet's |

## Auto update

Code: `src/updater.cpp` (network, parsing, hashing; no CEF code) and the "Auto update" section of `src/main.cpp` (state, UI, timers). The repository it checks is `UPDATE_REPO` in `src/resource.h`.

```
start ──10 s──► MaybeAutoCheck ──(installed copy, enabled, last check > 24 h)──► updater thread
                     ▲ hourly                                                     │
                                         GET api.github.com/repos/<repo>/releases/latest
                                                         │ newer tag?
                                         GET NeonNet-Setup.exe.sha256
                                         GET NeonNet-Setup.exe → Updates\NeonNet-Setup-<ver>.exe.part
                                                         │ SHA-256 matches, NEONPAK1 footer present
                                         rename to .exe, post U_READY to the UI thread
close ──► CefShutdown ──► UpdateShutdown: run "<installer> --silent --wait-pid=<pid> [--relaunch]"
```

Rules the updater follows:
- **HTTPS only.** Redirects to the GitHub CDN are followed; a downgrade to HTTP is refused (`WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP`).
- **Exact assets only.** The installer and checksum URLs must be exactly `https://github.com/<UPDATE_REPO>/releases/download/<tag>/NeonNet-Setup.exe` and `.sha256`. Assets from any other location are ignored.
- **Only newer versions.** The release tag (`v1.2.3`) is compared with `APP_VERSION`; equal or older versions are never installed.
- **Verified before use.** The file must match the SHA-256 from the release and end with the installer's `NEONPAK1` footer. Otherwise it is deleted.
- **Installed copy only.** Downloads and installs happen only when `NeonNet.exe` runs from the folder that the installer registered for this user (`InstallLocation` of the Uninstall entry in `HKCU`, or the default `%LOCALAPPDATA%\Programs\NeonNet`): `IsInstalledCopy()`. Other copies (for example `build/app`) and an install for all users (`IsMachineInstall()`, registered in `HKLM`) only report a newer version.
- **Off the UI thread.** The worker runs on its own thread and reports through `PostMessageW(WM_APP_UPDATE)`. It never touches the browser state. At exit it is cancelled and given up to 5 seconds to stop.

The checksum protects against corrupted or swapped downloads, but it lives in the same GitHub release as the installer. Anyone who can publish releases in the repository can publish an update. Signing the installer (see [Installer](INSTALLER.md#code-signing)) and checking the signature before running it would close that gap.

Registry values under `HKCU\Software\NeonNet`:

| Value | Meaning |
|---|---|
| `AutoUpdate` | 0 = automatic updates off (missing = on) |
| `LastUpdateCheck` | Time of the last successful check (milliseconds since 1601) |
| `LastRunVersion` | Version of the last run, used for the "UPDATED TO" notice |
| `UpdateResult` | Error text written by a failed silent install, shown once on the next start |

Leftover installers in `%LOCALAPPDATA%\NeonNet\Updates` are deleted at the next start.

## Shutdown

`WM_CLOSE` calls `BeginClose()`, which:
1. saves the window position;
2. hides the window immediately;
3. closes DevTools, then calls `CloseBrowser(true)` on every browser.

Each `OnBeforeClose` removes its browser from `g_all`. Once all are closed, `MaybeFinishClose()` calls `DestroyWindow`, followed by `CefQuitMessageLoop()` and `CefShutdown()`. All Chromium processes usually exit within about 4–5 seconds. If an update is ready, `UpdateShutdown()` then starts the installer, which waits for this process to end.

## Uninstall

`NeonNet.exe --uninstall` (called from Settings > Apps):
1. for an install for all users, when not elevated: starts itself again with the `runas` verb (UAC) and ends;
2. asks for confirmation;
3. removes the shortcuts (shared ones for an install for all users) and the registry keys: the Uninstall entry, the browser registration (`browserreg::Unregister`, also the one under `HKCU` that the settings button may have added) and `HKCU\Software\NeonNet`;
4. shows "NeonNet has been removed" and, only after it was dismissed, starts a hidden `cmd /c timeout 3 & rmdir` that deletes the app folder after the process exits (earlier the cleanup started first and lost the race when the message was read slowly). The folder is the registered `InstallLocation`, and is only deleted when the exe really is in it;
5. also deletes the profile, if the user chose to.

## Default browser registration

`src/browserreg.h` (header only, used by the app and the installer) writes the keys that make NeonNet appear in Settings > Default apps: the ProgIds `NeonNetURL` (with `URL Protocol`) and `NeonNetHTML` with `shell\open\command` = `"<exe>" "%1"`, `Software\Clients\StartMenuInternet\NeonNet` with `Capabilities`, and the value under `Software\RegisteredApplications`. They go under `HKCU` (an install for this user, and the settings button) or `HKLM` (an install for all users). A link or file then starts `NeonNet.exe "<url or path>"`: a new process hands the address to the running one (`OnAlreadyRunningAppRelaunch` -> `WM_APP_OPEN`) or starts with it (`NormalizeArgUrl()`, which turns an existing path into a `file:` address). Windows does not let a program become the default: that is stored in `UserChoice` under a hash only Windows' own pages can write, so NeonNet only opens `ms-settings:defaultapps?registeredAppUser=NeonNet` and reads `UserChoice` (`browserreg::IsDefault()`, both http and https must be `NeonNetURL`) to show the state.
