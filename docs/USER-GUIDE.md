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
| Navigation row | `[◀]` back, `[▶]` forward, `[⟳]` reload (turns into `[✕]` stop while loading), `[⌂]` home, address bar, `GO`, `SCAN`, `≡` menu |
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
| Anything else (`weather tokyo`) | Searched on Google |

Press Esc to cancel typing and restore the current page's address.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W or Ctrl+F4 | Close tab |
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

New tab, close tab, zoom, print, scanlines on/off, fullscreen, developer tools, the update options (see [Updates](#updates)), and **About NeonNet** (Chromium version and the shortcut list).

## Right-click on a page

The standard Chromium menu (back, reload, copy, paste and so on), plus:
- **Open link in new tab:** shown when you right-click a link.
- **Inspect:** opens the developer tools at the clicked element.

## Scanlines

The `SCAN` button turns a thin CRT line effect over every page on or off. It is purely visual: it never blocks clicks, and your choice is remembered.

## Downloads

NeonNet asks where to save each download. Progress appears in the status bar ("DOWNLOADING file-name 42%").

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
| Profile (history, cookies, cache, sign-ins) | `%LOCALAPPDATA%\NeonNet\Profile` |
| Downloaded updates (deleted after installing) | `%LOCALAPPDATA%\NeonNet\Updates` |
| Settings (scanlines, window position, automatic updates) | Registry `HKEY_CURRENT_USER\Software\NeonNet` |
| Shortcuts | Start menu `NeonNet`, and the desktop if chosen |

Session cookies (cookies without an expiry date) are cleared when NeonNet closes, as in Chrome's default setup. Normal sign-in cookies are kept.

## Uninstalling

Open **Settings > Apps > Installed apps > NeonNet Browser > Uninstall**. You are asked two things:
1. To confirm the uninstall.
2. Whether to also delete your browsing data (history, cookies, sign-ins). The default is **No**, so the data is still there if you install again later.

## FAQ

**Why is it so large?**
NeonNet carries a complete Chromium, so it does not depend on any other browser on the PC.

**Why doesn't Netflix play?**
Paid streaming services need the Widevine DRM module, which requires a separate license from Google. It is not included.

**Does NeonNet update itself?**
Yes, unless you turn off **Automatic updates** in the `≡` menu. See [Updates](#updates).

**Is it safe?**
NeonNet uses the same Chromium sandbox as Chrome. Its security depends on how recent the bundled Chromium is, so leave automatic updates on.

**I have an older NeonNet installed. What happens when I install this one?**
The installer uses the same folder and replaces the older version. Close the old NeonNet before installing.
