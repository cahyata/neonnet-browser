# NeonNet

NeonNet is a themed web browser for Windows. This file fixes the words used for its domain.

## Language

**Setting**:
A single choice the user makes that NeonNet remembers, with one name, one type, one default and one rule for which values are allowed. It also says whether an **Incognito window** may save it.
_Avoid_: option, preference, config key

**Action**:
Something the user asks for from a NeonNet page that happens once and leaves no value to remember, such as checking for an update. An action is not a **Setting**.
_Avoid_: command setting

**Incognito window**:
A window whose browsing leaves nothing behind once it is closed.
_Avoid_: private mode, ghost mode

## Websites

**Host**:
The name or IP address in a page's address, such as `news.example.co.uk`.
_Avoid_: domain

**Site**:
The part of a **Host** its owner registered, together with every host under it: `example.co.uk` for `news.example.co.uk`. An IP address is a site of its own.
_Avoid_: domain, root domain

**Origin**:
Protocol, **Host** and port together, such as `https://example.com`. Saved passwords and site permissions belong to exactly one origin.
_Avoid_: site (when one origin is meant)

**Site exception**:
A choice that one protection or behaviour does not apply to a **Site** or a **Host**, remembered until the user removes it.
_Avoid_: whitelist, exempt list
