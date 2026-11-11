# Security policy

## Supported versions

Only the latest NeonNet release receives fixes, including updates to the bundled Chromium. If you are on an older version, install the latest release from the Releases page first and check whether the problem is still there.

## Reporting a vulnerability

Please do not open a public issue for security problems.

Report them privately through GitHub instead: open the **Security** tab of this repository and choose **Report a vulnerability**. Include:

- the NeonNet version (shown in **About NeonNet**, `≡` menu);
- what you did, what happened, and what you expected;
- steps or a page that reproduces it, if you have them.

## What belongs where

| Problem | Where to report |
|---|---|
| NeonNet's own code: the window, tabs, the `neonnet.ui` pages and command channel, the installer, the uninstaller | Here, as described above |
| Chromium itself, for example a renderer or JavaScript engine bug that also affects Chrome | The Chromium project at https://issues.chromium.org |
| An outdated bundled Chromium | Here. NeonNet updates by moving to a newer Chromium Embedded Framework build. |
