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
  | `history/delete?id=`, `history/clear` | Remove one visit, or all |
  | `bookmarks` | All bookmarks, oldest first |
  | `bookmarks/delete?id=`, `bookmarks/edit?id=&title=&url=` | Change bookmarks; `edit` rejects anything that is not http, https or file |
  | `downloads` | Up to 300 downloads, newest first (state, sizes, speed, `exists`, `openable`) |
  | `downloads/remove?id=`, `downloads/clear` | Remove one finished download from the list, or all finished ones |
  | `downloads/cancel?id=`, `downloads/open?id=`, `downloads/show?id=` | Posted to the UI thread as `WM_APP_DOWNLOAD` (see Downloads below) |
  | `settings` | `{"search": "<search URL prefix>"}` for the new tab page |

  The API answers only a **main frame that shows a `https://neonnet.ui/` page** in a tab (403 otherwise). A website's `fetch`, even with `no-cors`, cannot read or change the lists. Changes made through the API post `WM_APP_STORE` to the UI thread, which saves and refreshes the star.
- **Pages:** `history.html`, `bookmarks.html` and the new tab page fetch this API and build their lists with `textContent`. Links are only made for http, https and file URLs.

## Toolbar host and zoom

The toolbar (`chrome.html`) is served from `https://chrome.neonnet.ui/`, a second host handled by the same `UiSchemeFactory`, while the pages (new tab, history, bookmarks, downloads) stay on `https://neonnet.ui/`. Chromium stores zoom per host. On one shared host, zooming a page also zoomed the toolbar, which sits in a fixed-height area, so it shrank or was clipped. Its `/cmd` requests are relative, so they go to the same host. The `api/` calls still require a main frame on `https://neonnet.ui/`, so the toolbar host cannot use them.

## Downloads

`OnBeforeDownload` and `OnDownloadUpdated` in `Client` feed `store::DownloadUpdate()`, which creates the entry the first time it sees a CEF download id (so a download is listed as soon as the save dialog opens) and then updates sizes, speed and state. The list is written to `downloads.txt` (`time TAB url TAB name TAB path TAB total TAB state`) like the others, but only when a state changes (start, finish, cancel): saving on every progress report would keep restarting the 2-second save timer. Entries loaded from disk that were still running become `DL_INTERRUPTED`. At most 500 entries are kept.

`ServeApi()` runs on the IO thread, but cancelling needs the `CefDownloadItemCallback`, which `OnDownloadUpdated` keeps in `g.dlCallbacks` on the UI thread. So `downloads/cancel`, `open` and `show` only post `WM_APP_DOWNLOAD` with the entry id, and `DoDownloadAction()` does the work on the UI thread. The page never sends a path: the file is looked up by id in the list, must exist and be finished, and `OPEN` is refused for programs and scripts (`store::DownloadOpenable()`). `SHOW` runs `explorer.exe /select,"<path>"`. `downloads.html` polls `/api/downloads` once a second while it is visible and updates rows in place, so buttons stay clickable.

`OnContextInitialized` also sets the Chromium preference `download_bubble.partial_view_enabled` to false (the "Don't show when downloads finish" option). Without it Chromium pops up its own unthemed download bubble after every download. A `disable-features=DownloadBubble` switch did not remove it; the preference does.

## Search engine

`SEARCH_ENGINES` in `main.cpp` lists the engines. The chosen index is `g.engine` (atomic, because the IO thread reads it for `/api/settings`) and is saved as the registry string `SearchEngine` (`google`, `duckduckgo`, ...). The `≡` menu item `Search engine` (`CMD_SEARCH`) switches to the next one. The state sent to the chrome UI carries `search`, the URL prefix that `chrome.html` appends the encoded query to; `newtab.html` reads it from `/api/settings`.

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
- **Installed copy only.** Downloads and installs happen only when `NeonNet.exe` runs from `%LOCALAPPDATA%\Programs\NeonNet`. Other copies (for example `build/app`) only report a newer version.
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
1. asks for confirmation;
2. removes the shortcuts and registry keys;
3. starts a hidden `cmd /c timeout 3 & rmdir` that deletes the app folder after the process exits. The folder is only deleted when the exe really is in the standard install folder;
4. also deletes the profile, if the user chose to.
