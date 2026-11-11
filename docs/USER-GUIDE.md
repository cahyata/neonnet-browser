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
- To upgrade, run the installer of the newer version. Files are replaced and your browsing data is kept.

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

New tab, close tab, zoom, print, scanlines on/off, fullscreen, developer tools, and **About NeonNet** (Chromium version and the shortcut list).

## Right-click on a page

The standard Chromium menu (back, reload, copy, paste and so on), plus:
- **Open link in new tab:** shown when you right-click a link.
- **Inspect:** opens the developer tools at the clicked element.

## Scanlines

The `SCAN` button turns a thin CRT line effect over every page on or off. It is purely visual: it never blocks clicks, and your choice is remembered.

## Downloads

NeonNet asks where to save each download. Progress appears in the status bar ("DOWNLOADING file-name 42%").

## Where things are stored

| What | Location |
|---|---|
| Application | `%LOCALAPPDATA%\Programs\NeonNet` |
| Profile (history, cookies, cache, sign-ins) | `%LOCALAPPDATA%\NeonNet\Profile` |
| Settings (scanlines, window position) | Registry `HKEY_CURRENT_USER\Software\NeonNet` |
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
No. Install a newer version by running its installer.

**Is it safe?**
NeonNet uses the same Chromium sandbox as Chrome. Its security depends on how recent the bundled Chromium is, so keep NeonNet up to date.

**I have an older NeonNet installed. What happens when I install this one?**
The installer uses the same folder and replaces the older version. Close the old NeonNet before installing.
