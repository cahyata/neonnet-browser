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
| Status bar | Left: download and upload speed of the computer, then the status (LINK STABLE, LOADING..., SIGNAL LOST, the hovered link address, or download progress). Right: zoom (when not 100%), SCAN ON and the clock |

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

## Performance panel

Ctrl+Shift+M, the `PERF` button next to `SCAN`, or **Performance panel** in the `≡` menu opens a panel on the right side of the window; the page gets narrower. Press the same keys, or `✕` in the panel, to close it. While it is closed it uses no memory itself.

- **Memory and CPU:** the total NeonNet uses, split into **pages** (the tabs), **browser** (the window itself) and **engine** (graphics, network and other helper processes), and the CPU use of everything, as a share of the whole processor.
- **Tabs:** each tab with its memory and CPU, largest first. A tab using at least 20% of the CPU is marked **HOT** and listed on top. A tab that shares a process with another one shows the shared figure with a `~`. Hover a row for the speaker (mute), the lock (keep this site awake), the moon (put to sleep) and the cross (close). Small tags show why a tab is protected: **SOUND**, **TYPING**, **AWAKE**.
- **Clean memory:** each page collects its garbage, then NeonNet asks Windows to take back memory it no longer needs. The number drops at once, and rises again as pages are used: it helps most when another program needs the RAM. Putting tabs to sleep frees much more.
- **Sleep background tabs / sleeping tabs:** a sleeping tab is closed to free its memory. It stays in the tab strip, dimmed and in italics, and loads again, at the same address, when you select it. What you had typed into a page, or a video playing in it, is lost when it goes to sleep, so tabs that have either are protected (see below). The tab you are looking at never sleeps.
- **Tabs that stay awake:** "Sleep background tabs" and the RAM limiter skip a tab that **plays sound**, one with **typed text** in a text field, text area or editable area (the check runs half a second after you stop typing, and the tab is free again once the field is empty or the page changes), and any tab of a site on the **kept awake** list. Put a site on the list with the lock on one of its tabs; it is shown under **KEPT AWAKE** in the panel, where `✕` takes it off. A notice in the status bar tells how many tabs were kept awake. You can still put a protected tab to sleep yourself with its moon button.
- **RAM limiter:** off, or 1, 2, 3, 4, 6 or 8 GB. While everything NeonNet uses is above the limit, the background tab unused the longest is put to sleep, one every few seconds, until the total is under the limit or only the current tab is left. A notice in the status bar says which tab it was. A tab that is loading is skipped.
- **CPU limiter:** background tabs keep working (a download, a chat, a build log), but with low priority and in Windows' power-saving mode, so they take less CPU and give way to what you are using. The selected tab always runs normally.

The limiters are remembered between runs and work with the panel closed. Windows' own "efficiency mode" needs Windows 10 version 1709 or later; without it only the lower priority applies.

## Sound and mute

A tab that is playing sound shows a speaker next to its number in the tab strip. Click the speaker to mute the tab: it turns into a crossed-out speaker that stays until you click it again, and the tab stays muted even when it goes to sleep and wakes up. The same button is in the performance panel.

Sound is detected from audio and video elements on the page that are playing, not muted and really producing audio, within a second or two. Sound made only through the Web Audio API (some games and synthesizers) is not detected, so such a tab shows no speaker, but muting still works on it.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+Shift+M | Performance panel |
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
| F12 | Developer tools (docked in the window, press again to close; choose the position in the tools' three-dot menu > Dock side, or in Settings > DEVELOPER TOOLS) |
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

## Smooth scrolling

The mouse wheel and the keyboard scroll with an animation instead of jumping: each wheel step glides to its place, and fast turning adds up. It works for arrow keys, space and Shift+space, page up and down, home and end as well. **Settings > SCROLLING** has the switch, the **scroll distance** (short, normal or long: how far one wheel step moves the page) and the **smoothness** (snappy, smooth or very smooth: how long the glide takes). A change applies to the pages that are open, no reload needed.

It stays out of the way where it would do harm: a touchpad (already smooth), a page that handles the wheel itself (maps, editors), text fields, video and audio controls, and scrolling with Ctrl, Shift or Alt held (zoom and sideways scrolling). Only normal web pages are smoothed; NeonNet's own pages scroll as usual.

## Site information and permissions

The lock icon at the left of the address bar opens a panel for the page you are on. It says whether the connection is secure (https), not secure (http) or opened despite a certificate warning, and lists what the site may use: **location, camera, microphone, notifications, clipboard and motion sensors**. For each one choose **ASK** (NeonNet asks when the site wants it), **ALLOW** or **BLOCK**. **RESET THIS SITE** forgets all choices for the site; **ALL SITES** opens the full list at `neonnet://permissions`, also reachable from **Settings > SITE PERMISSIONS**.

When a site asks for something, the answer you give is remembered for that site. A change reaches a page that is already open when you reload it. Incognito windows keep answers in memory until the window closes.

## Network speed

The bottom left of the status bar shows the download speed (cyan, `▼`) and upload speed (pink, `▲`), in B/s, KB/s or MB/s, updated every second. It is the traffic of the whole computer on its Ethernet, Wi-Fi and mobile data adapters together, so a download in another program shows up too. Virtual adapters (VMware, VirtualBox, Hyper-V, VPN tunnels) are left out so nothing is counted twice. Switch it off in **Settings > LOOK > Network speed in the status bar**.

## Certificate warnings and HTTPS-only mode

When a site shows a certificate NeonNet cannot trust (expired, made for another name, issued by an unknown authority ...), the page is not loaded. A warning explains the problem. **GO BACK TO SAFETY** is the default button. **ADVANCED** shows the reason and a button to open the site anyway. That choice lasts for that site until NeonNet is closed, and nothing is saved. Anything you type into such a site can be read by an attacker.

**HTTPS-only mode** (Settings > SECURITY, off by default) opens every `http://` address over `https://`. If the secure address fails, a warning says the site is not secure, with **GO BACK TO SAFETY** as the default and a button to continue over plain http for that site until NeonNet is closed. Addresses on this computer or the local network (`localhost`, `*.local`, IP addresses and one-word names) are never changed.

## Blocking trackers and ads

Turn it on in **Settings > SECURITY** or with **Block trackers and ads** in the `≡` menu. It is off by default, because a blocker can break a site now and then. When it is on, a request that a page makes to a known advertising or tracking site is cancelled, so the ad or tracker never loads. The list has about 82 000 domains, from EasyList, EasyPrivacy and Peter Lowe's list. What is never blocked: the page you open, and requests a site makes to itself or to its own parts (`example.com` and `cdn.example.com`). Reload a page after changing the switch. The settings page counts the requests blocked since NeonNet started. There is no per-site exception yet: if a site breaks, switch the blocker off for a moment.

## Saved passwords

After you send a login form, and the page that follows no longer asks for a password, NeonNet asks **Save password?** with **SAVE**, **NEVER FOR THIS SITE** and **NOT NOW** (the question says **UPDATE** when the user name is already saved with another password). Later, click into the user name or password field of that site: a small list of saved user names opens under the field. Click one and the user name and password are filled in. NeonNet never fills a password without that click, and only for the exact site (address and port) it was saved for.

Passwords are stored in `%LOCALAPPDATA%\NeonNet\passwords.dat`, encrypted with Windows DPAPI: only your Windows account on this computer can read them, so copying the file elsewhere is of no use. `neonnet://passwords` (or **MANAGE** in the settings) lists the sites and user names, deletes one or all, and shows the sites you chose **NEVER** for. It cannot show a password. **Settings > PASSWORDS > Offer to save passwords** turns the question off; saved passwords can still be filled. An incognito window never saves or fills. Chromium's own password manager and address and card autofill are turned off, so no other copy of a password is kept.

Limits: sign-in on sites that ask in a separate pop-up (HTTP authentication) is not supported, see [Page dialogs](#page-dialogs-and-permission-prompts). Logins done with a script and no click or Enter key (and sites that change the page without leaving the form on screen) may not be offered.

## Page dialogs and permission prompts

When a page shows an alert, a question (OK / CANCEL), a box to type in, or asks "leave this site?", the dialog opens in the middle of the window in the NeonNet theme, with the name of the site in its title. The window behind it waits until you answer. Enter presses the highlighted button, Esc is the same as CANCEL (or STAY). In a prompt the text is selected, so you can type over it.

A **permission prompt** appears when a site wants to use your camera, microphone, location, notifications, the clipboard, or another feature of your device. It lists what it wants, and has **ALLOW** and **BLOCK**. Enter blocks; Esc dismisses the question without an answer (the site is told the permission was denied, and may ask again later). Your answer is remembered until NeonNet is closed, so the same site is not asked twice. Nothing is saved to disk: after a restart sites ask again.

Not covered: a site that asks for a **user name and password** (HTTP sign-in) fails with a SIGNAL LOST page in this version, because the engine does not hand these requests to NeonNet. File choosers, print and the colour picker are windows of Windows or of Chromium.

## Pinned tabs and moving tabs

- **Reorder:** press a tab and drag it left or right. The other tabs make room as you pass them. Clicking a tab still selects it at once.
- **Pin:** right-click a tab and choose **Pin tab** (**Unpin tab** to undo). A pinned tab moves to the left end of the strip and becomes narrow, showing a pin and its number; hover it for the title. It has no close button and a middle click does not close it, but Ctrl+W and **Close tab** in its menu do. Pinned tabs can only be dragged among themselves, and other tabs cannot be dropped among them.
- **Tab menu:** besides pinning it has **Duplicate tab**, **Mute tab** / **Unmute tab** (when the tab has sound), **Close other tabs** (pinned tabs stay open) and **Close tab**.

## Tabs of the last session

Pinned tabs always come back when NeonNet starts. If **Continue where you left off** is on in the settings, all tabs of the last session come back, in their order, with the same tab selected. They come back asleep (dimmed): only the selected tab loads, the others load when you click them, so a session of thirty tabs starts as fast as one. The back/forward history, scroll position and what you typed into pages are not restored. If NeonNet is started with an address (a link from another program), that page opens as a new tab as well. An incognito window never saves or restores tabs. The tabs are saved a few seconds after each change and when NeonNet closes, so after a crash you lose at most a few seconds. The list is in `%LOCALAPPDATA%\NeonNet\session.txt`; delete the file to forget it.

## Reopening closed tabs

Ctrl+Shift+T reopens the most recently closed tab at its old position. Press it again for the one before, up to 25 tabs back. This works while NeonNet is open; the list starts empty at each start. The page reloads, but its back/forward history is not restored.

## Bookmarks

- **Add or remove:** press Ctrl+D or click `☆` in the address bar. The star turns yellow (`★`) on bookmarked pages. NeonNet's own pages cannot be bookmarked.
- **Bookmarks page:** Ctrl+Shift+O, the `≡` menu, or **BOOKMARKS** on the new tab page. Search, open, **EDIT** (title and address) or **DELETE** a bookmark there.
- **New tab page:** once you have bookmarks, your nine newest replace the default quick links.

## History

Every page you open is recorded with its title and time, for 90 days (up to 20,000 visits). Pages that failed to load are left out.

Open it with Ctrl+H, the `≡` menu, or **HISTORY** on the new tab page. You can search it, remove single visits with `✕`, or click **CLEAR BROWSING DATA**, which opens the privacy section of the settings page with only the history ticked. There you choose the period (last hour to all time). Clearing the history does not clear cookies or the cache unless you tick them too.

## Settings

Ctrl+, , the `≡` menu (**Settings**), or **SETTINGS** on the new tab page opens the settings page. Every change is saved and applied at once.

| Setting | What it does |
|---|---|
| **Search engine** | Google, DuckDuckGo, Bing, Brave or Ecosia. Used for text that is not an address, in the address bar and on the new tab page. |
| **Home page** | Opened at start-up (when NeonNet is started without an address) and by the home button. Empty means the NeonNet new tab page. Only `http://`, `https://` and `file:///` addresses are accepted. New tabs always show the new tab page. |
| **Site icons on tabs** | On (default): each tab shows the site's own icon between its number and its title (a pinned tab shows it in place of the pin). The icon is fetched from the site by the toolbar page, like an image on a web page. Off: no icons, nothing is fetched. |
| **HTTPS-only mode**, **Block trackers and ads** | See [Certificate warnings and HTTPS-only mode](#certificate-warnings-and-https-only-mode) and [Blocking trackers and ads](#blocking-trackers-and-ads). Both are off by default. |
| **Offer to save passwords** | On (default): NeonNet asks to save a password after a login. See [Saved passwords](#saved-passwords). |
| **Smooth scrolling**, **Scroll distance**, **Smoothness** | See [Smooth scrolling](#smooth-scrolling). On by default. |
| **Network speed in the status bar** | On (default). See [Network speed](#network-speed). |
| **Continue where you left off** | Off (default): NeonNet starts with the home page. On: the tabs of the last session come back, asleep, and the home page is not opened. Pinned tabs come back either way. See [Tabs of the last session](#tabs-of-the-last-session). |
| **Clear browsing data** | Deletes cookies and site data, the cache and the browsing history, in any combination, for the last hour, 24 hours, 7 days, 4 weeks or all time. History goes by the time of the visit. Cookies and site data also sign you out of sites. Cookies go by the time they were created. Site data (local storage, offline data) is cleared for the sites that lost a cookie, the sites in your history for that period and the pages open now, so it can miss a site you have not visited recently. The cache is always cleared completely. Single visits, or everything at once, can also be removed on the history page. In an incognito window the incognito session is cleared. |
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
| Saved passwords (encrypted) | `%LOCALAPPDATA%\NeonNet\passwords.dat` |
| Tabs of the last session | `%LOCALAPPDATA%\NeonNet\session.txt` |
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
