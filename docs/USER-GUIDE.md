# User guide

## Installing

1. Run `NeonNet-Setup.exe`.
2. Choose the two options before installing:
   - **Create desktop shortcut:** adds a shortcut to the desktop.
   - **Launch NeonNet when done:** opens NeonNet right after installing.
3. Click **INSTALL**. Unpacking the Chromium runtime takes about 10–30 seconds.
4. When **INSTALL COMPLETE** appears, click **LAUNCH** or **FINISH**.

Good to know:
- NeonNet is installed for the current Windows account only, so no admin rights are needed.
- It needs about 450 MB of disk space.
- If NeonNet is running, the installer asks you to close it first.
- Later versions install themselves (see [Updates](#updates)). You can also run a newer installer by hand; files are replaced and your browsing data is kept.

Windows SmartScreen may warn that the app is unrecognized, because the installer is not signed with a code signing certificate. Click **More info**, then **Run anyway**, if you trust where the file came from.

## The window

| Part | What it does |
|---|---|
| Top row | Logo, tab list (numbered 01, 02, ...), `+` button, and the minimize, maximize and close buttons |
| Navigation row | `[◀]` back, `[▶]` forward, `[⟳]` reload (turns into `[✕]` stop while loading), `[⌂]` home, address bar with the `☆` bookmark star, `GO`, `SCAN`, `≡` menu |
| Find bar | Shown under the navigation row while you search the page (see [Find in page](#find-in-page)) |
| Thin line under the address bar | Animated pink-cyan bar while a page is loading |
| Status bar | Left: status (LINK STABLE, LOADING..., SIGNAL LOST, the hovered link address, or download progress). Right: zoom (when not 100%), SCAN ON and the clock |

Moving and resizing:
- **Move:** drag an empty part of the tab row, or the status bar.
- **Maximize:** double-click an empty part of the tab row.
- **Resize:** drag a window edge or corner.

## Address bar

| You type | What happens |
|---|---|
| A full address (`https://...`) | Opened as is |
| A domain (`github.com`, `wikipedia.org/wiki/Neon`) | Opened with `https://` |
| `localhost:3000` or an IP address | Opened with `http://` |
| `neonnet://settings` (also `history`, `bookmarks`, `downloads`) | One of NeonNet's own pages. The address bar shows this short form on those pages. |
| Anything else (`weather tokyo`) | Searched with your search engine (Google until you change it, see [Search engine](#search-engine)) |

Press Esc to cancel typing and restore the current page's address.

### Suggestions

While you type, a list opens under the address bar with matching bookmarks (`★`) and pages from your history (`⏱`), best match first. The first row is always what Enter does with the text as typed (open it, or search for it).

- **Up / Down:** move through the list. The address bar shows the chosen address; Enter opens it. Going up past the first entry brings back what you typed.
- **Click** a row to open it.
- **Esc:** closes the list and keeps your text. Esc again restores the page's address.
- **Inline completion:** when the best match is a secure address that starts with what you typed, the rest is filled in and selected (`git` becomes `git`**`hub.com`**). Press Enter to accept it, or keep typing or press Backspace to ignore it.

Matches must contain every word you typed, in the address or the title. Pages you visit often, recently, or have bookmarked rank higher. Suggestions come only from your own bookmarks and history on this computer; nothing you type is sent to a server for them. Text you submit as a search goes to your search engine, as before.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W or Ctrl+F4 | Close tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Shift+N | New incognito window (in an incognito window: a new tab) |
| Ctrl+F | Find in page |
| F3, Ctrl+G / Shift+F3, Ctrl+Shift+G | Next / previous match |
| Ctrl+D | Bookmark the page, or remove its bookmark |
| Ctrl+Shift+O | Bookmarks |
| Ctrl+H | History |
| Ctrl+J | Downloads |
| Ctrl+, | Settings |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+1 … Ctrl+8 | Go to tab 1–8 |
| Ctrl+9 | Go to the last tab |
| Ctrl+L, F6 | Focus the address bar |
| F5, Ctrl+R | Reload |
| Alt+Left / Alt+Right | Back / forward |
| Alt+Home | New tab page |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in / zoom out / reset zoom |
| Ctrl+P | Print |
| F11 | Fullscreen (Esc to leave) |
| F12 | Developer tools |
| Middle-click a tab | Close that tab |

## The `≡` menu

New tab, close tab, reopen closed tab, bookmark this page, bookmarks, history, find in page, zoom, print, scanlines on/off, fullscreen, developer tools, the update options (see [Updates](#updates)), and **About NeonNet** (Chromium version and the shortcut list).

Use the mouse, or the arrow keys and Enter. Esc, a click outside the menu, or another click on `≡` closes it.

## Right-click on a page

The menu is drawn in the NeonNet theme, like the `≡` menu: arrows and Enter, or the mouse; Esc or a click elsewhere closes it. It holds the usual commands for what you clicked (back, reload, save, print, copy, paste, select all and so on), plus:
- **Open link in new tab** and **Open link in incognito window:** shown when you right-click a link.
- **Open image in new tab:** shown when you right-click an image.
- **Search <engine> for "...":** shown when text is selected. It uses the search engine from Settings.
- **View page source:** shown when you right-click empty page space; opens in a new tab.
- **Open in system player:** shown when you right-click a video or audio player on a page. See [Videos NeonNet cannot play](#videos-neonnet-cannot-play).
- **Inspect:** opens the developer tools at the clicked element.

## Videos NeonNet cannot play

NeonNet does not include the H.264 and AAC codecs, because they need patent licences. Most `.mp4`, `.mov` and `.m4a` files use them. VP9, AV1, Opus and MP3 work, so YouTube and most streaming sites are fine.

- **A link straight to a video file:** NeonNet asks what to do. **Yes** opens it in your media player (VLC, mpv, Windows' own player, whatever is set for that file type), **No** saves it, **Cancel** does nothing.
- **A video inside a page that fails:** the status bar shows `VIDEO FORMAT NOT SUPPORTED`. Click play on the video and it opens in your media player (a right-click and **Open in system player** does the same).
- **Temporary copy:** the player gets a copy in `%LOCALAPPDATA%\NeonNet\Temp\media`. NeonNet deletes it about two minutes after opening it, once no program holds it open: a player that is still showing the video keeps the file locked, so it stays until you close it. Anything left over is removed the next time NeonNet starts. If no program can open the file, it is moved to your Downloads folder instead.
- **Files you download on purpose** (a Download button, or a server that marks the file as an attachment) are saved as usual, without a question.
- **DRM:** when a site asks for DRM (Widevine and similar), the status bar shows `THIS SITE NEEDS DRM`. Protected video cannot be opened in a player either; it only exists encrypted.

## Incognito window

Ctrl+Shift+N, or **New incognito window** in the `≡` menu, opens a second NeonNet window with a green frame and an INCOGNITO badge.

- **Not saved:** cookies, cache, local storage and sign-ins live in memory only, so you are signed out of everything and nothing is left when the window closes. No browsing history is recorded.
- **Kept:** files you download stay on disk (the downloads page lists them until the window closes). Settings you change, such as the search engine, are shared with the normal window.
- **Bookmarks** are shown, so the address bar and new tab page still suggest them, but they cannot be added, changed or removed from an incognito window. The history menu entry is turned off.
- **Several incognito tabs share one session.** Pressing Ctrl+Shift+N again, or starting NeonNet with the incognito option while it is open, adds a tab to the open incognito window. The normal window and its cookies are never touched, and the two do not see each other's sign-ins.
- **Updates** are checked by the normal window only.
- **What it does not do:** websites, your network and your internet provider can still see what you visit.

## Find in page

Ctrl+F opens the find bar under the address bar. Matches are highlighted as you type, and the counter shows which match is selected (`3/17`, or `0/0` in pink when nothing matches).

- **Enter** or `[▼]`: next match. **Shift+Enter** or `[▲]`: previous match.
- **Aa:** match upper and lower case exactly.
- **Esc** or `[✕]`: close the bar.

The find bar belongs to the tab it was opened in and closes when you switch to another tab. Ctrl+F or F3 brings it back with the last search.

## Reopening closed tabs

Ctrl+Shift+T reopens the most recently closed tab at its old position. Press it again for the one before, up to 25 tabs back. This works while NeonNet is open; the list starts empty at each start. The page reloads, but its back/forward history is not restored.

## Bookmarks

- **Add or remove:** press Ctrl+D or click `☆` in the address bar. The star turns yellow (`★`) on bookmarked pages. NeonNet's own pages cannot be bookmarked.
- **Bookmarks page:** Ctrl+Shift+O, the `≡` menu, or **BOOKMARKS** on the new tab page. Search, open, **EDIT** (title and address) or **DELETE** a bookmark there.
- **New tab page:** once you have bookmarks, your nine newest replace the default quick links.

## History

Every page you open is recorded with its title and time, for 90 days (up to 20,000 visits). Pages that failed to load are left out.

Open it with Ctrl+H, the `≡` menu, or **HISTORY** on the new tab page. You can search it, remove single visits with `✕`, or click **CLEAR ALL** twice to remove everything. Clearing the history does not clear cookies or the cache.

## Settings

Ctrl+, , the `≡` menu (**Settings**), or **SETTINGS** on the new tab page opens the settings page. Every change is saved and applied at once.

| Setting | What it does |
|---|---|
| **Search engine** | Google, DuckDuckGo, Bing, Brave or Ecosia. Used for text that is not an address, in the address bar and on the new tab page. |
| **Home page** | Opened at start-up (when NeonNet is started without an address) and by the home button. Empty means the NeonNet new tab page. Only `http://`, `https://` and `file:///` addresses are accepted. New tabs always show the new tab page. |
| **Clear browsing data** | Deletes cookies and site data, the cache, or both, for the last hour, 24 hours, 7 days, 4 weeks or all time. Cookies and site data also sign you out of sites. Cookies go by the time they were created. Site data (local storage, offline data) is cleared for the sites that lost a cookie, the sites in your history for that period and the pages open now, so it can miss a site you have not visited recently. The cache is always cleared completely. History has its own button on the history page. In an incognito window the incognito session is cleared. |
| **Ask where to save each file** | On (default): a save dialog opens for every download. Off: files go straight into your Downloads folder, and a number is added when the name is taken (`report (1).pdf`). |
| **Scanlines** | The same switch as the `SCAN` button. |
| **Automatic updates** | See [Updates](#updates). Only the installed copy updates itself, so the switch is disabled in a copy started from another folder. |

The page also shows the NeonNet and Chromium versions.

## Scanlines

The `SCAN` button turns a thin CRT line effect over every page on or off. It is purely visual: it never blocks clicks, and your choice is remembered.

## Downloads

NeonNet asks where to save each download. Progress appears in the status bar ("DOWNLOADING file-name 42%").

Ctrl+J, the `≡` menu, or **DOWNLOADS** on the new tab page opens the downloads page. It lists every download, newest first, with size, speed, source and time. You can search it, and each row has buttons:

- **CANCEL** stops a running download.
- **OPEN** opens a finished file with its default program.
- **FOLDER** shows the file in File Explorer.
- `✕` removes the row from the list. The file itself is kept.

**CLEAR LIST** (click twice) removes every finished row; running downloads stay. If a file was moved or deleted, its row says FILE MISSING.

For safety, **OPEN** is not offered for programs and scripts (`.exe`, `.msi`, `.bat`, `.cmd`, `.ps1`, `.vbs`, `.js`, `.lnk`, `.dll` and similar). Use FOLDER and check the file first. A download that was still running when NeonNet closed is listed as INTERRUPTED the next time.

## Updates

NeonNet updates itself:

1. About 10 seconds after it starts, and then once a day while it runs, NeonNet asks GitHub for the latest release.
2. If there is a newer version, it downloads the installer in the background. The status bar shows "DOWNLOADING UPDATE v1.0.1 42%".
3. The download is checked against the SHA-256 checksum published with the release. A file that does not match is deleted.
4. When it is ready, the status bar shows "UPDATE v1.0.1 READY - INSTALLS WHEN NEONNET CLOSES". The update is installed after you close NeonNet, and takes about 10–30 seconds.
5. The next time you open NeonNet, the status bar briefly shows "UPDATED TO NEONNET 1.0.1".

Options in the `≡` menu:

| Item | What it does |
|---|---|
| Restart to update to v… | Shown when an update is ready. Closes NeonNet, installs the update and opens NeonNet again. |
| Check for updates | Checks right away and shows the result in the status bar |
| Automatic updates | Turns the daily check on or off |

If an update fails, the status bar says so on the next start, and NeonNet tries again later.

Privacy: each check sends one request to `api.github.com`, and downloads come from GitHub. GitHub sees your IP address, like for any website. No other data is sent. Turn off **Automatic updates** to stop the checks.

Automatic updates only work for the installed copy (in `%LOCALAPPDATA%\Programs\NeonNet`). A copy started from anywhere else only tells you that a new version exists.

## Where things are stored

| What | Location |
|---|---|
| Application | `%LOCALAPPDATA%\Programs\NeonNet` |
| Profile (cookies, cache, sign-ins, site data) | `%LOCALAPPDATA%\NeonNet\Profile` |
| History, bookmarks and downloads list | `%LOCALAPPDATA%\NeonNet\history.txt`, `bookmarks.txt` and `downloads.txt` (plain UTF-8 text) |
| Downloaded updates (deleted after installing) | `%LOCALAPPDATA%\NeonNet\Updates` |
| Settings (scanlines, search engine, home page, download behaviour, window position, automatic updates) | Registry `HKEY_CURRENT_USER\Software\NeonNet` |
| Shortcuts | Start menu `NeonNet`, and the desktop if chosen |

Session cookies (cookies without an expiry date) are cleared when NeonNet closes, as in Chrome's default setup. Normal sign-in cookies are kept.

## Uninstalling

Open **Settings > Apps > Installed apps > NeonNet Browser > Uninstall**. You are asked two things:
1. To confirm the uninstall.
2. Whether to also delete your browsing data (history, bookmarks, cookies, sign-ins). The default is **No**, so the data is still there if you install again later.

## FAQ

**Why is it so large?**
NeonNet carries a complete Chromium, so it does not depend on any other browser on the PC.

**Why doesn't Netflix play?**
Paid streaming services need the Widevine DRM module, which requires a separate license from Google. It is not included, and NeonNet shows a warning in the status bar when a site asks for it. They also use H.264 and AAC, which NeonNet leaves out for licensing reasons.

**Does NeonNet update itself?**
Yes, unless you turn off **Automatic updates** in the `≡` menu. See [Updates](#updates).

**Is it safe?**
NeonNet uses the same Chromium sandbox as Chrome. Its security depends on how recent the bundled Chromium is, so leave automatic updates on.

**I have an older NeonNet installed. What happens when I install this one?**
The installer uses the same folder and replaces the older version. Close the old NeonNet before installing.
