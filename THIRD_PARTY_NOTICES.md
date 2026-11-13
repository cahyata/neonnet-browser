# Third-party notices

NeonNet includes the following third-party software. This file is installed next to `NeonNet.exe`.

## Chromium Embedded Framework (CEF)

- Version: see `cef_version.txt` in the source repository, or **About NeonNet** in the app
- Project: https://github.com/chromiumembedded/cef
- License: BSD 3-Clause. The full text is in `LICENSE-CEF.txt` in the install folder.

```
Copyright (c) 2008-2020 Marshall A. Greenblatt. Portions Copyright (c)
2006-2009 Google Inc. All rights reserved.
```

`NeonNet.exe` is the CEF sandbox bootstrap executable (`bootstrap.exe`), renamed and given the NeonNet icon and version information.

## Chromium

- Project: https://www.chromium.org
- License: BSD 3-Clause for Chromium itself, plus the licenses of the many open-source components it includes (V8, Skia, ICU, FFmpeg, and others).
- The complete list of components and their license texts is in `CREDITS.html` in the install folder.

Chromium is a trademark of Google LLC. NeonNet is not affiliated with or endorsed by Google.

## Filter lists (tracker and ad blocking)

`ui/blocklist.txt`, built into NeonNet, is a list of domain names made by `tools/make_blocklist.py` from:

- **EasyList** and **EasyPrivacy** - https://easylist.to - dual licensed under the GNU GPL v3 or later and Creative Commons Attribution-ShareAlike 3.0. Only domain names taken from the lists' rules are used. The resulting list stays under the same licenses.
- **Peter Lowe's ad server list** - https://pgl.yoyo.org/adservers/ - free to use.
