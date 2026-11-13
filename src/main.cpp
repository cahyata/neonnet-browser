// NeonNet - synthwave / cyber-HUD themed browser for Windows.
//
// Built as NeonNet.dll and started by CEF's sandbox bootstrap (bootstrap.exe
// renamed to NeonNet.exe), which calls RunWinMain below for the browser process
// and every sub-process (renderer, GPU, ...). Chromium ships inside the app
// folder, so nothing depends on Edge or the WebView2 runtime.
//
// Layout: the window frame and status bar are painted with GDI; the chrome UI
// (tabs, address bar) is a CEF browser showing ui/chrome.html; each tab is a
// CEF browser created as a child window below it.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <mutex>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_scheme.h"
#include "include/cef_v8.h"
#include "resource.h"
#include "updater.h"
#include "store.h"
#include "perf.h"
#include "crash.h"
#include "userscripts.h"
#include "browserreg.h"
#include <windowsx.h>   // after CEF headers: its macros (GetFirstChild...) clash with cef_dom.h

// ---------------------------------------------------------------------------
// Constants

static const int BORDER_DIP = 5;
static const int CHROME_DIP = 84;
static const int BM_BAR_DIP = 28; // bookmarks bar row under the address bar
static const int STATUS_DIP = 24;
static const int CUT_DIP = 16;
static const int FIND_DIP = 34;   // find bar row, added under the chrome UI while open

static const COLORREF C_BG = RGB(0x12, 0x06, 0x2c);
static const COLORREF C_PINK = RGB(0xff, 0x2e, 0x97);
static const COLORREF C_CYAN = RGB(0x00, 0xe5, 0xff);
static const COLORREF C_YELLOW = RGB(0xff, 0xd6, 0x00);
static const COLORREF C_MUTED = RGB(0x8a, 0x6f, 0xc0);
static const COLORREF C_GREEN = RGB(0x35, 0xff, 0xa0);   // accent of an incognito window
static const cef_color_t CEF_BG = 0xFF12062C;

static const wchar_t* UI_HOST = L"https://neonnet.ui/";
static const wchar_t* NEWTAB = L"https://neonnet.ui/newtab.html";
// The toolbar lives on its own host: Chromium keeps zoom per host, so on neonnet.ui it would
// shrink or grow whenever the user zooms the new tab, history or downloads page.
static const wchar_t* CHROME_UI_HOST = L"https://chrome.neonnet.ui/";
static const wchar_t* CHROME_UI = L"https://chrome.neonnet.ui/chrome.html";
// The performance panel: a second UI browser docked to the right of the page. Like the toolbar it
// lives on the chrome.neonnet.ui host (own zoom, and allowed to send commands).
static const wchar_t* PANEL_UI = L"https://chrome.neonnet.ui/performance.html";
static const int PANEL_DIP = 340;
static const wchar_t* HISTORY_PAGE = L"https://neonnet.ui/history.html";
static const wchar_t* BOOKMARKS_PAGE = L"https://neonnet.ui/bookmarks.html";
static const wchar_t* DOWNLOADS_PAGE = L"https://neonnet.ui/downloads.html";
static const wchar_t* SETTINGS_PAGE = L"https://neonnet.ui/settings.html";
static const wchar_t* EXTENSIONS_PAGE = L"https://neonnet.ui/extensions.html";
static const wchar_t* APP_DIR_NAME = L"NeonNet";
static const wchar_t* REG_KEY = L"Software\\NeonNet";
static const wchar_t* UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NeonNet";
static const wchar_t* SHORTCUT_NAME = L"NeonNet.lnk";

static const UINT WM_APP_CMD = WM_APP + 1;      // lParam = new std::wstring (chrome UI command)
static const UINT WM_APP_OPEN = WM_APP + 2;     // lParam = new std::wstring (url from a 2nd instance)
static const UINT WM_APP_UPDATE = WM_APP + 3;   // lParam = new UpdateMsg (from the updater thread)
static const UINT WM_APP_STORE = WM_APP + 4;    // history/bookmarks changed by a neonnet.ui page
static const UINT WM_APP_FINISH = WM_APP + 5;   // all browsers closed: destroy the window (posted, not called from OnBeforeClose)
static const UINT WM_APP_DOWNLOAD = WM_APP + 6; // wParam = DlAction, lParam = download id (from the downloads page)
static const UINT WM_APP_SETTING = WM_APP + 7;  // lParam = const std::pair<std::wstring, std::wstring>* (key, value; from the settings page)
static const UINT WM_APP_MEDIA = WM_APP + 8;    // lParam = new MediaAsk (a video NeonNet cannot play was opened)
static const UINT WM_APP_CTXMENU = WM_APP + 9;  // lParam = new CtxMenu (a right-click menu to show in the NeonNet theme)
static const UINT WM_APP_CLEARDATA = WM_APP + 10;   // lParam = new ClearReq (from the settings page)
static const UINT WM_APP_DIALOG = WM_APP + 12;      // lParam = new DlgReq (a page dialog, permission prompt or sign-in to show in the NeonNet theme)
static const UINT WM_APP_DEVOPEN = WM_APP + 13;     // wParam = tab id: open its developer tools again (after a change of their position)
static const UINT WM_APP_CLEARSITES = WM_APP + 11;  // lParam = new ClearSites (cookies are done, site data is next)

static const UINT_PTR TIMER_CLOCK = 1, TIMER_ZOOM = 2, TIMER_UPDATE = 3, TIMER_NOTICE = 4, TIMER_SAVE = 5,
                      TIMER_ALERT = 6, TIMER_TEMP = 7, TIMER_PERF = 8, TIMER_TRIM = 9, TIMER_AUDIO = 10,
                      TIMER_SESSION = 11, TIMER_NET = 12;
static const ULONGLONG UPDATE_INTERVAL_MS = 24ull * 60 * 60 * 1000;   // check at most once a day

enum Cmd {
    CMD_NEWTAB = 1, CMD_CLOSETAB, CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SCAN,
    CMD_FULLSCREEN, CMD_DEVTOOLS, CMD_ABOUT, CMD_RELOAD, CMD_STOP, CMD_BACK, CMD_FWD,
    CMD_FOCUSURL, CMD_NEXTTAB, CMD_PREVTAB, CMD_HOME, CMD_PRINT,
    CMD_UPDATE_CHECK, CMD_UPDATE_AUTO, CMD_UPDATE_RESTART, CMD_UPDATE_PAGE,
    CMD_FIND, CMD_FINDNEXT, CMD_FINDPREV, CMD_REOPEN, CMD_BOOKMARK, CMD_BOOKMARKS, CMD_BOOKMARKS_BAR, CMD_HISTORY,
    CMD_DOWNLOADS, CMD_SETTINGS, CMD_INCOGNITO, CMD_PERFORMANCE, CMD_BLOCKADS, CMD_EXTENSIONS, CMD_HELP
};
enum DlAction { DLA_CANCEL = 1, DLA_OPEN, DLA_SHOW };

// Updater states (see "Auto update" below)
enum UpdState { U_IDLE, U_CHECKING, U_DOWNLOADING, U_READY, U_AVAILABLE, U_UPTODATE, U_FAILED };
struct UpdateMsg {
    UpdState state = U_IDLE;
    int percent = 0;
    bool manual = false;
    std::wstring version, path, page, error;
};
enum { MENU_OPEN_LINK_TAB = MENU_ID_USER_FIRST, MENU_INSPECT, MENU_OPEN_PLAYER,
       MENU_VIEW_SOURCE, MENU_OPEN_IMAGE_TAB, MENU_OPEN_LINK_INCOGNITO, MENU_SEARCH_SELECTION };

// A login the user just sent, waiting to see whether it worked before NeonNet offers to save it.
struct PwPending { std::wstring origin, user, pass; ULONGLONG at = 0; };

struct Tab {
    int id = 0;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefBrowser> dev;   // its developer tools (null = closed)
    int devDock = 0;             // where they are: 0 bottom, 1 right, 2 left, 3 own window
    bool devReopen = false;      // being closed only to open again at another kind of place (window <-> docked)
    RECT devHole{};              // docked tools: where they leave room for the page, in pixels from the page area's corner
    bool devHoleSet = false;     // ... once they have said so
    std::wstring title = L"NEW TAB";
    std::wstring url;
    std::wstring errorUrl;
    bool loading = true;
    bool canBack = false, canFwd = false;
    bool errorPending = false;
    long long histId = 0;   // history entry of the page shown
    // performance panel
    DWORD pid = 0;                 // renderer process of the page (reported by the renderer itself)
    bool discarded = false;        // unloaded to free memory: no browser until the tab is selected again
    bool discarding = false;       // CloseBrowser() called by UnloadTab(): the tab itself stays
    unsigned long long memBytes = 0;
    double cpu = 0;
    ULONGLONG lastActive = 0;      // GetTickCount64() when it was last the selected tab
    bool muted = false;            // audio muted by the user (kept when the tab sleeps and wakes)
    ULONGLONG audioUntil = 0;      // the page said it is playing sound: audible until this tick (see MEDIA_HOOK_JS)
    bool audible = false;          // as last shown to the UI
    bool dirty = false;            // a text field on the page holds text the user typed: do not unload it
    bool netEnabled = false;       // DevTools Network.enable sent to this browser
    int netLimitKB = -1;           // download cap currently applied in KB/s (-1 = unlimited)
    PwPending pw;
    std::wstring upgradedFrom, upgradedTo;   // HTTPS-only mode: the http address asked for, and the https address tried instead
    std::wstring icon;             // address of the page's favicon (http/https), shown in the tab strip
    bool pinned = false;           // pinned tabs sit at the left, are narrow and come back at the next start
    int wsId = 1;                  // workspace this tab belongs to
};

struct ClosedTab { std::wstring url; int index; bool pinned = false; int wsId = 1; };

struct Workspace {
    int id = 1;
    std::wstring name = L"MAIN";
    std::wstring icon = L"\u25C6";
    std::wstring color = L"#00e5ff";
    int activeTabId = 0;
};

// Something Chromium would ask the user in a window of its own: a page's alert, confirm or prompt,
// the "leave this site?" question, or a permission prompt (camera, location ...). The
// browser callback hands over one of these; it is shown in the NeonNet theme later, from the
// window procedure (never inside the callback), and the callback is answered afterwards.
struct DlgReq {
    enum Kind { ALERT, CONFIRM, PROMPT, UNLOAD, PERM, MEDIA, PWSAVE } kind = ALERT;
    int tabId = 0;
    std::wstring origin, message, defText, secret;   // secret: the password to save (PWSAVE), wiped after use
    bool isReload = false;
    uint32_t perms = 0;
    CefRefPtr<CefJSDialogCallback> js;
    CefRefPtr<CefPermissionPromptCallback> perm;
    CefRefPtr<CefMediaAccessCallback> media;
};

struct App {
    HMODULE hmod = nullptr;   // NeonNet.dll (resources live here)
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    CefRefPtr<CefBrowser> chrome;
    std::atomic<int> chromeId{ 0 };
    // performance panel
    CefRefPtr<CefBrowser> panel;
    std::atomic<int> panelId{ 0 };
    bool panelOpen = false, creatingPanel = false;
    perf::Snapshot perfSnap;
    unsigned long long perfTotal = 0, perfBrowser = 0;   // bytes: all processes, the browser process alone
    double perfCpu = 0;
    DWORD ramLimitMB = 0;          // 0 = off: unload idle background tabs while all processes together use more
    bool bgLimit = false;          // run background tabs at low priority
    DWORD netLimitKB = 0;          // 0 = off: cap download speed in KB/s via DevTools network conditions
    bool netLimitAll = false;      // false = background tabs only, true = all tabs
    std::set<std::wstring> keepHosts;   // sites whose tabs never go to sleep on their own (registry KeepAwakeHosts)
    int sleepBlocked = 0;          // background tabs the RAM limiter would have slept but must not (audio, typing, kept awake)
    int autoUnloaded = 0;          // tabs unloaded by the RAM limiter since NeonNet started
    ULONGLONG lastLimitAct = 0;
    int lastTrimCount = 0;
    std::vector<std::unique_ptr<Tab>> tabs;
    std::vector<Workspace> workspaces;
    int activeWs = 1, nextWsId = 2;
    std::string lastWorkspaces;
    std::deque<int> pendingPopups;  // tab ids waiting for their popup browser
    int creatingTab = 0;            // tab id while CreateBrowserSync runs
    bool creatingChrome = false;
    int active = 0, nextId = 1;
    std::atomic<bool> scan{ true };   // atomic: the IO thread reads it for /api/settings
    int dropX0 = 0, dropX1 = 0;       // horizontal extent of that dropdown, CSS px (0, 0 = the whole width)
    int dropNeed = 0;               // address bar suggestions open: toolbar height needed, CSS px (0 = closed)
    bool fsUser = false, fsElement = false, fullscreen = false;
    WINDOWPLACEMENT fsPrev{ sizeof(WINDOWPLACEMENT) };
    std::wstring hoverText, download;
    std::wstring alert;   // urgent status bar message (unsupported video / DRM), shown above everything else
    std::wstring startUrl;
    bool favicons = true;             // show site icons on tabs (registry ShowFavicons)
    bool bookmarksBar = true;         // show the bookmarks bar under the address bar (registry BookmarksBar)
    bool httpsOnly = false;           // HTTPS-only mode (registry HttpsOnly)
    bool savePasswords = true;        // offer to save passwords (registry SavePasswords)
    bool smooth = true;               // smooth scrolling (registry SmoothScroll), with the distance of a wheel step in px (100 = the normal step)
    int devDock = 0;                  // where developer tools open: 0 bottom, 1 right, 2 left, 3 separate window (registry DevToolsDock)
    int smoothStep = 120, smoothTime = 400;  // and the length of the animation in ms
    bool netSpeed = true;             // network speed in the status bar (registry NetSpeed)
    double netDown = 0, netUp = 0;    // bytes per second, last second
    ULONGLONG netTick = 0;
    std::map<DWORD, std::pair<DWORD, DWORD>> netPrev;   // per adapter: last in and out byte counters
    std::atomic<bool> blockAds{ false };   // block trackers and ads (registry BlockAds); atomic: read on the IO thread
    std::atomic<int> blockedCount{ 0 };    // requests blocked since NeonNet started
    std::set<std::wstring> certAllow;  // host:port the user chose to open despite a certificate error, until NeonNet closes
    std::set<std::wstring> httpAllow;  // hosts the user chose to open over plain http (HTTPS-only mode), until NeonNet closes
    std::set<std::wstring> warned;     // what NeonNet itself showed a warning for ("cert|host:port", "http|host"): only these can be proceeded
    std::deque<std::unique_ptr<DlgReq>> dlgQueue;   // dialogs waiting for the one on screen
    bool dlgBusy = false;
    bool sessionPending = false;      // a write of the open tabs to session.txt is scheduled
    std::string lastSession;          // what session.txt holds now, so an unchanged strip is not written again
    std::wstring lastStateJson, cachedBmBarJson;
    bool bmBarDirty = true, lastZoomed = false;
    int rgnW = -1, rgnH = -1; bool rgnSimple = false;
    bool closing = false;
    // Incognito window (own process, started with --neonnet-incognito): in-memory browser
    // profile, no history, no saved window placement, no updates. Set once before CEF starts.
    bool incognito = false;
    CefRefPtr<CefRequestContext> reqCtx;   // incognito: the in-memory context every tab uses (null = normal profile)
    int devtools = 0;
    // auto update
    UpdState upd = U_IDLE;
    int updPercent = 0;
    std::wstring updVersion, updPath, updPage, notice;
    bool updRelaunch = false;
    std::atomic<bool> updCancel{ false };
    HANDLE updThread = nullptr;
    // find in page
    bool findOpen = false, findCase = false;
    std::wstring findText;
    // search engine used for text typed in the address bar (index into SEARCH_ENGINES;
    // atomic because the IO thread answers /api/settings)
    std::atomic<int> engine{ 0 };
    // "Clear browsing data": 0 idle, 1 running, 2 done, with what the last run removed (read by the IO thread)
    std::atomic<int> clearState{ 0 }, clearCookies{ 0 }, clearSites{ 0 }, clearHistory{ 0 };
    // running downloads (UI thread only): CEF id -> handle used to cancel them
    std::map<unsigned, CefRefPtr<CefDownloadItemCallback>> dlCallbacks;
    // "Open in system player": CEF download id -> temporary file, while it downloads; and the
    // time each finished temporary file was handed to a program (UI thread only)
    std::map<unsigned, std::wstring> tempDl;
    std::map<std::wstring, ULONGLONG> tempOpened;
    std::wstring pendingPlayerUrl;   // media URL to download as a temporary copy for the system player
    std::wstring pendingSaveUrl;     // media URL the user chose to save: no second question
    // recently closed tabs, newest last
    std::vector<ClosedTab> closed;
};
static App g;
static std::mutex g_updMu;
struct UpdSnap {
    UpdState state = U_IDLE;
    int percent = 0;
    std::wstring version, result, error;
};
static UpdSnap g_updSnap;
static void SyncUpdSnap(const wchar_t* result = nullptr, const std::wstring& err = L"") {
    std::lock_guard<std::mutex> lk(g_updMu);
    g_updSnap.state = g.upd;
    g_updSnap.percent = g.updPercent;
    g_updSnap.version = g.updVersion;
    if (result) { g_updSnap.result = result; g_updSnap.error = err; }
}
static COLORREF Accent() { return g.incognito ? C_GREEN : C_PINK; }   // window frame, status bar line, menu marks

// Search engines for text typed in the address bar. `id` is what is saved in the registry.
struct SearchEngine { const wchar_t* id; const wchar_t* name; const wchar_t* url; };
static const SearchEngine SEARCH_ENGINES[] = {
    { L"google",     L"Google",     L"https://www.google.com/search?q=" },
    { L"duckduckgo", L"DuckDuckGo", L"https://duckduckgo.com/?q=" },
    { L"bing",       L"Bing",       L"https://www.bing.com/search?q=" },
    { L"brave",      L"Brave",      L"https://search.brave.com/search?q=" },
    { L"ecosia",     L"Ecosia",     L"https://www.ecosia.org/search?q=" },
};
static const int SEARCH_ENGINE_COUNT = (int)(sizeof(SEARCH_ENGINES) / sizeof(SEARCH_ENGINES[0]));
static const SearchEngine& CurrentEngine() {
    int i = g.engine.load();
    return SEARCH_ENGINES[i >= 0 && i < SEARCH_ENGINE_COUNT ? i : 0];
}

// ---------------------------------------------------------------------------
// Helpers

static int S(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

static std::wstring JsonStr(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        case 0x2028: o += L"\\u2028"; break;
        case 0x2029: o += L"\\u2029"; break;
        default:
            if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", c); o += b; }
            else o += c;
        }
    }
    return o + L"\"";
}

static std::wstring HtmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring PercentDecode(const std::wstring& in) {
    std::string bytes;
    std::string raw = Utf8(in);
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '%' && i + 2 < raw.size() && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
            char hex[3] = { raw[i + 1], raw[i + 2], 0 };
            bytes += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (raw[i] == '+') bytes += ' ';
        else bytes += raw[i];
    }
    return Wide(bytes);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static DWORD RegGetDword(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    return v;
}

static void RegSetBin(const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, type, (const BYTE*)data, size);
        RegCloseKey(k);
    }
}

static std::wstring DownloadTarget(std::wstring name);   // below, next to ApplySetting
static void ShowAlert(const std::wstring& text);
static void LaunchIncognito(const std::wstring& url);
static void ApplyBackgroundLimit();   // performance panel, below
static void ApplyNetworkLimit();
static void PushPerf();
static std::wstring HostOf(Tab* t);
static const wchar_t* SleepProtection(Tab* t);
static std::wstring TempMediaPath(const std::wstring& name, const std::wstring& mime);
static void OpenTempMedia(const std::wstring& path);
// Clear browsing data (settings page). cutoffMs is the unix time (ms) from which data is removed, 0 = all time.
struct ClearReq { long long cutoffMs = 0; bool cookies = false, cache = false, history = false; };
struct ClearSites { long long cutoffMs = 0; int cookies = 0; bool history = false; std::set<std::wstring> hosts; };

// A video NeonNet cannot play. `callback` is set when Chromium already started a download for it;
// otherwise the video was opened as a page and `browser` starts the download after the answer.
struct MediaAsk {
    CefRefPtr<CefBeforeDownloadCallback> callback;
    unsigned id = 0;
    std::wstring url, name, mime;
    CefRefPtr<CefBrowser> browser;
};

// History and bookmarks are written to disk shortly after they change.
static void ScheduleSave() { if (g.hwnd) SetTimer(g.hwnd, TIMER_SAVE, 2000, nullptr); }

// The open tabs are written to session.txt a few seconds after they change (not on every change,
// and not again while the timer is already running), so a crash loses at most a few seconds.
static void ScheduleSession() {
    if (!g.hwnd || g.closing || g.incognito || g.sessionPending) return;
    g.sessionPending = true;
    SetTimer(g.hwnd, TIMER_SESSION, 3000, nullptr);
}

static Tab* FindTab(int id) { for (auto& t : g.tabs) if (t->id == id) return t.get(); return nullptr; }
static Tab* TabOf(CefRefPtr<CefBrowser> b) {
    if (!b) return nullptr;
    int bid = b->GetIdentifier();
    for (auto& t : g.tabs) if (t->browser && t->browser->GetIdentifier() == bid) return t.get();
    return nullptr;
}
static bool IsChrome(CefRefPtr<CefBrowser> b) { return b && b->GetIdentifier() == g.chromeId.load(); }
static bool IsPanel(CefRefPtr<CefBrowser> b) { return b && g.panelId.load() && b->GetIdentifier() == g.panelId.load(); }
static bool IsUiBrowser(CefRefPtr<CefBrowser> b) { return IsChrome(b) || IsPanel(b); }   // NeonNet's own UI, never a page
static Tab* Active() { return FindTab(g.active); }
static int IndexOf(int id) { for (size_t i = 0; i < g.tabs.size(); i++) if (g.tabs[i]->id == id) return (int)i; return -1; }

// Pinned tabs always form the left end of the strip.
static int PinnedCount() { int n = 0; for (auto& t : g.tabs) if (t->pinned) n++; return n; }
static void KeepPinsFirst() {
    std::stable_partition(g.tabs.begin(), g.tabs.end(), [](const std::unique_ptr<Tab>& t) { return t->pinned; });
}

static bool LoadUi(int id, std::string& out) {
    HRSRC r = FindResourceW(g.hmod, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL h = r ? LoadResource(g.hmod, r) : nullptr;
    const char* p = h ? (const char*)LockResource(h) : nullptr;
    if (!p) return false;
    out.assign(p, SizeofResource(g.hmod, r));
    return true;
}

// ---------------------------------------------------------------------------
// Geometry

static int Frame() { return (IsZoomed(g.hwnd) || g.fullscreen) ? 0 : S(BORDER_DIP); }
static RECT ChromeRect() {
    RECT rc; GetClientRect(g.hwnd, &rc); int b = Frame();
    return { b, b, rc.right - b, b + S(CHROME_DIP + (g.bookmarksBar ? BM_BAR_DIP : 0) + (g.findOpen ? FIND_DIP : 0)) };
}
// The toolbar browser grows downwards over the page while the address bar suggestions are
// open, and is raised above the page for it. ContentRect() does not change, so the page
// underneath is not re-laid out.
static RECT ChromeHostRect() {
    RECT r = ChromeRect();
    if (g.dropNeed > 0 && !g.fullscreen) {
        RECT rc; GetClientRect(g.hwnd, &rc);
        r.bottom = std::min((int)rc.bottom - Frame(), std::max((int)r.bottom, (int)r.top + S(g.dropNeed)));
    }
    return r;
}
static RECT StatusRect() { RECT rc; GetClientRect(g.hwnd, &rc); int b = Frame(); return { b, rc.bottom - b - S(STATUS_DIP), rc.right - b, rc.bottom - b }; }
static RECT ContentRect() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    if (g.fullscreen) return rc;
    RECT c = ChromeRect(), s = StatusRect();
    int right = c.right;
    if (g.panelOpen) right = std::max((int)c.left + S(200), (int)c.right - S(PANEL_DIP));   // the panel takes the right edge
    return { c.left, c.bottom, right, s.top };
}
// The performance panel: right of the page, same height.
static RECT PanelRect() {
    RECT c = ChromeRect(), s = StatusRect(), p = ContentRect();
    return { p.right, c.bottom, c.right, s.top };
}

static void UpdateRegion() {
    bool simple = IsZoomed(g.hwnd) || g.fullscreen || IsIconic(g.hwnd);
    if (simple) {
        if (!g.rgnSimple) { SetWindowRgn(g.hwnd, nullptr, TRUE); g.rgnSimple = true; g.rgnW = g.rgnH = -1; }
        return;
    }
    RECT wr; GetWindowRect(g.hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top, c = S(CUT_DIP);
    if (!g.rgnSimple && w == g.rgnW && h == g.rgnH) return;
    g.rgnSimple = false; g.rgnW = w; g.rgnH = h;
    POINT pts[6] = { {0, 0}, {w - c, 0}, {w, c}, {w, h}, {c, h}, {0, h - c} };
    SetWindowRgn(g.hwnd, CreatePolygonRgn(pts, 6, WINDING), TRUE);
}

static void Place(CefRefPtr<CefBrowser> b, const RECT& r, bool visible, bool raise = false, bool bottom = false) {
    if (!b) return;
    HWND w = b->GetHost()->GetWindowHandle();
    if (!w) return;
    bool wasVisible = IsWindowVisible(w) != FALSE;
    if (!visible && !wasVisible) return;   // hidden background tab already hidden: skip SetWindowPos
    if (visible) {
        SetWindowPos(w, bottom ? HWND_BOTTOM : raise ? HWND_TOP : nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     ((raise || bottom) ? 0 : SWP_NOZORDER) | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!wasVisible) b->GetHost()->WasHidden(false);
    } else {
        ShowWindow(w, SW_HIDE);
        b->GetHost()->WasHidden(true);
    }
}

static void Layout() {
    if (!g.hwnd) return;
    if (g.chrome) {
        Place(g.chrome, ChromeHostRect(), !g.fullscreen, g.dropNeed > 0);
        // A dropdown that is narrower than the toolbar (the site panel): only the toolbar row and the panel
        // itself belong to the toolbar window, so the page stays visible beside the panel.
        if (HWND cw = g.chrome->GetHost()->GetWindowHandle()) {
            RECT r = ChromeHostRect(), nav = ChromeRect();
            if (g.dropNeed > 0 && g.dropX1 > g.dropX0 && !g.fullscreen) {
                int w = r.right - r.left, navH = nav.bottom - nav.top, h = r.bottom - r.top;
                HRGN a = CreateRectRgn(0, 0, w, navH), b = CreateRectRgn(std::max(0, S(g.dropX0)), navH, std::min(w, S(g.dropX1)), h);
                CombineRgn(a, a, b, RGN_OR);
                DeleteObject(b);
                SetWindowRgn(cw, a, TRUE);
            } else SetWindowRgn(cw, nullptr, TRUE);
        }
    }
    if (g.panel) Place(g.panel, PanelRect(), g.panelOpen && !g.fullscreen);
    RECT content = ContentRect();
    for (auto& t : g.tabs) {
        RECT pr = content;
        if (t->dev && t->devDock != 3 && !g.fullscreen) {
            // Developer tools docked in the window: they fill the whole area, underneath the page, and draw a hole where
            // the page belongs (a bottom, right or left dock, or the device toolbar's frame, only change that hole).
            Place(t->dev, content, t->id == g.active, false, true);
            if (t->devHoleSet) {
                pr.left = std::min(std::max(content.left + t->devHole.left, content.left), content.right - 1);
                pr.top = std::min(std::max(content.top + t->devHole.top, content.top), content.bottom - 1);
                pr.right = std::max(std::min(content.left + t->devHole.right, content.right), pr.left + 1);
                pr.bottom = std::max(std::min(content.top + t->devHole.bottom, content.bottom), pr.top + 1);
            }
        }
        Place(t->browser, pr, t->id == g.active);
    }
    UpdateRegion();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void InvalidateStatus() { RECT s = StatusRect(); InvalidateRect(g.hwnd, &s, FALSE); }

// ---------------------------------------------------------------------------
// Chrome UI sync

static std::wstring DisplayUrl(Tab* t) { return t->errorUrl.empty() ? t->url : t->errorUrl; }

static void RunInChrome(const std::wstring& js) {
    if (g.chrome) g.chrome->GetMainFrame()->ExecuteJavaScript(js, CHROME_UI, 0);
}

static std::wstring SiteJson(const std::wstring& url);   // the site panel's data, below with the site permissions

static Workspace* FindWs(int id) {
    for (auto& w : g.workspaces) if (w.id == id) return &w;
    return nullptr;
}

static void EnsureDefaultWorkspaces() {
    if (g.workspaces.empty()) {
        Workspace w; w.id = 1; w.name = L"MAIN"; w.icon = L"\u25C6"; w.color = L"#00e5ff";
        g.workspaces.push_back(std::move(w));
        g.activeWs = 1;
        if (g.nextWsId < 2) g.nextWsId = 2;
    }
    if (!FindWs(g.activeWs)) g.activeWs = g.workspaces.front().id;
}

static std::wstring WorkspacesPath() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\workspaces.txt";
}

static std::wstring SanitizeWsField(std::wstring s, size_t maxLen) {
    for (auto& c : s) if (c < 0x20 || c == L'\t') c = L' ';
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    while (!s.empty() && s.back() == L' ') s.pop_back();
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

static void SaveWorkspaces() {
    if (g.incognito) return;
    EnsureDefaultWorkspaces();
    std::string out = "NEONNET-WORKSPACES 1\n" + std::to_string(g.activeWs) + "\t" + std::to_string(g.nextWsId) + "\n";
    for (auto& w : g.workspaces) {
        out += std::to_string(w.id) + "\t" + Utf8(w.icon) + "\t" + Utf8(w.color) + "\t" + Utf8(w.name) + "\n";
    }
    if (out == g.lastWorkspaces) return;
    std::wstring path = WorkspacesPath(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    bool ok = WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr) && wr == out.size();
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) g.lastWorkspaces = out;
    else DeleteFileW(tmp.c_str());
}

static void LoadWorkspaces() {
    g.workspaces.clear();
    g.activeWs = 1;
    g.nextWsId = 2;
    if (!g.incognito) {
        HANDLE h = CreateFileW(WorkspacesPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, nullptr), got = 0;
            std::string data;
            if (size > 0 && size < 256 * 1024) {
                data.resize(size);
                if (!ReadFile(h, &data[0], size, &got, nullptr) || got != size) data.clear();
            }
            CloseHandle(h);
            size_t pos = data.find('\n');
            if (pos != std::string::npos && data.compare(0, pos, "NEONNET-WORKSPACES 1") == 0) {
                g.lastWorkspaces = data;
                size_t nl2 = data.find('\n', ++pos);
                if (nl2 != std::string::npos) {
                    std::string hdr = data.substr(pos, nl2 - pos);
                    size_t tb = hdr.find('\t');
                    if (tb != std::string::npos) {
                        int aw = atoi(hdr.substr(0, tb).c_str());
                        int nw = atoi(hdr.substr(tb + 1).c_str());
                        if (aw > 0) g.activeWs = aw;
                        if (nw > 1) g.nextWsId = nw;
                    }
                    pos = nl2 + 1;
                }
                for (; pos < data.size() && g.workspaces.size() < 12;) {
                    size_t e = data.find('\n', pos);
                    if (e == std::string::npos) e = data.size();
                    std::string line = data.substr(pos, e - pos);
                    pos = e + 1;
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    size_t t1 = line.find('\t');
                    size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
                    size_t t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
                    if (t3 == std::string::npos) continue;
                    Workspace w;
                    w.id = atoi(line.substr(0, t1).c_str());
                    w.icon = SanitizeWsField(Wide(line.substr(t1 + 1, t2 - t1 - 1)), 4);
                    w.color = SanitizeWsField(Wide(line.substr(t2 + 1, t3 - t2 - 1)), 16);
                    w.name = SanitizeWsField(Wide(line.substr(t3 + 1)), 24);
                    if (w.id <= 0 || w.name.empty()) continue;
                    if (w.icon.empty()) w.icon = L"\u25C6";
                    if (w.color.empty() || w.color[0] != L'#') w.color = L"#00e5ff";
                    if (w.id >= g.nextWsId) g.nextWsId = w.id + 1;
                    if (!FindWs(w.id)) g.workspaces.push_back(std::move(w));
                }
            }
        }
    }
    EnsureDefaultWorkspaces();
}

static void SendState() {
    EnsureDefaultWorkspaces();
    Tab* a = Active();
    if (a) SetWindowTextW(g.hwnd, ((a->title.empty() ? std::wstring(L"New tab") : a->title) +
                                   (g.incognito ? L" - NeonNet (Incognito)" : L" - NeonNet")).c_str());
    if (!g.chrome) return;
    std::wstring j = L"{\"type\":\"state\",\"tabs\":[";
    bool firstTab = true;
    for (size_t i = 0; i < g.tabs.size(); i++) {
        Tab* t = g.tabs[i].get();
        if (t->wsId != g.activeWs) continue;
        if (!firstTab) j += L",";
        firstTab = false;
        j += L"{\"id\":" + std::to_wstring(t->id) + L",\"title\":" + JsonStr(t->title) +
             L",\"loading\":" + (t->loading ? L"true" : L"false") + L",\"discarded\":" + (t->discarded ? L"true" : L"false") +
             L",\"audible\":" + (t->audible ? L"true" : L"false") + L",\"muted\":" + (t->muted ? L"true" : L"false") +
             L",\"pinned\":" + (t->pinned ? L"true" : L"false") +
             (g.favicons && !t->icon.empty() ? L",\"icon\":" + JsonStr(t->icon) : L"") + L"}";
    }
    j += L"],\"active\":" + std::to_wstring(g.active);
    j += L",\"activeWs\":" + std::to_wstring(g.activeWs) + L",\"workspaces\":[";
    for (size_t i = 0; i < g.workspaces.size(); i++) {
        const auto& w = g.workspaces[i];
        int cnt = 0;
        for (auto& t : g.tabs) if (t->wsId == w.id) cnt++;
        if (i) j += L",";
        j += L"{\"id\":" + std::to_wstring(w.id) + L",\"name\":" + JsonStr(w.name) +
             L",\"icon\":" + JsonStr(w.icon) + L",\"color\":" + JsonStr(w.color) +
             L",\"count\":" + std::to_wstring(cnt) + L"}";
    }
    j += L"]";
    ScheduleSession();   // every change of the tab strip (and of titles and addresses) ends up here
    j += L",\"url\":" + JsonStr(a ? DisplayUrl(a) : L"");
    j += std::wstring(L",\"canBack\":") + (a && a->canBack ? L"true" : L"false");
    j += std::wstring(L",\"canFwd\":") + (a && a->canFwd && a->errorUrl.empty() ? L"true" : L"false");
    j += std::wstring(L",\"loading\":") + (a && a->loading ? L"true" : L"false");
    j += std::wstring(L",\"scan\":") + (g.scan ? L"true" : L"false");
    j += std::wstring(L",\"favicons\":") + (g.favicons ? L"true" : L"false");
    j += std::wstring(L",\"incognito\":") + (g.incognito ? L"true" : L"false");
    j += std::wstring(L",\"perf\":") + (g.panelOpen ? L"true" : L"false");
    j += std::wstring(L",\"max\":") + (IsZoomed(g.hwnd) ? L"true" : L"false");
    std::wstring cur = a ? DisplayUrl(a) : L"";
    j += std::wstring(L",\"canStar\":") + (!g.incognito && store::Recordable(cur) ? L"true" : L"false");
    j += std::wstring(L",\"starred\":") + (store::FindBookmark(cur) ? L"true" : L"false");
    j += std::wstring(L",\"bookmarksBar\":") + (g.bookmarksBar ? L"true" : L"false");
    if (g.bookmarksBar && (g.bmBarDirty || g.cachedBmBarJson.empty())) {
        g.cachedBmBarJson = Wide(store::BookmarksBarJson(L""));
        g.bmBarDirty = false;
    }
    j += L",\"bar\":" + (g.bookmarksBar ? g.cachedBmBarJson : L"[]");
    j += std::wstring(L",\"find\":") + (g.findOpen ? L"true" : L"false");
    j += std::wstring(L",\"findCase\":") + (g.findCase ? L"true" : L"false");
    j += L",\"findText\":" + JsonStr(g.findText);
    j += L",\"search\":" + JsonStr(CurrentEngine().url);
    j += L",\"site\":" + SiteJson(cur);
    j += L"}";
    if (j == g.lastStateJson) return;
    g.lastStateJson = j;
    RunInChrome(L"window.__nn&&window.__nn(" + j + L")");
}

static void FocusPage() { if (Tab* a = Active()) if (a->browser) a->browser->GetHost()->SetFocus(true); }
static void FocusUrl() {
    if (!g.chrome) return;
    g.chrome->GetHost()->SetFocus(true);
    RunInChrome(L"window.__nn&&window.__nn({type:'focusUrl'})");
}

// ---------------------------------------------------------------------------
// Find in page: the find bar is a third row of the chrome UI, shown while g.findOpen.

static void FindResult(int count, int active) {
    RunInChrome(L"window.__nn&&window.__nn({type:'findResult',count:" + std::to_wstring(count) +
                L",active:" + std::to_wstring(active) + L"})");
}

static void DoFind(bool forward) {
    Tab* a = Active();
    if (!a || !a->browser) return;
    if (g.findText.empty()) { a->browser->GetHost()->StopFinding(true); FindResult(0, 0); return; }
    // Always findNext=true: a search started with false only highlights the matches
    // without selecting one, so the first "next" would select match 1 again. A changed
    // text or match-case setting restarts the search either way.
    a->browser->GetHost()->Find(g.findText, forward, g.findCase, true);
}

static void Layout();
static void OpenFind() {
    bool was = g.findOpen;
    g.findOpen = true;
    if (!was) { Layout(); SendState(); }
    if (g.chrome) {
        g.chrome->GetHost()->SetFocus(true);
        RunInChrome(L"window.__nn&&window.__nn({type:'focusFind'})");
    }
    if (!was && !g.findText.empty()) DoFind(true);
}

static void CloseFind() {
    if (!g.findOpen) return;
    g.findOpen = false;
    if (Tab* a = Active()) if (a->browser) a->browser->GetHost()->StopFinding(true);
    Layout();
    SendState();
    FocusPage();
}

// ---------------------------------------------------------------------------
// Scanlines and error page

static std::wstring ScanScript(bool on) {
    std::wstring s = L"(()=>{try{if(window.top!==window)return;const ID='__neonnet_scan';const on=";
    s += on ? L"true" : L"false";
    s += L";function ap(){const e=document.getElementById(ID);"
         L"if(!on){if(e)e.remove();return;}if(e)return;"
         L"const d=document.createElement('div');d.id=ID;const st=d.style;"
         L"st.position='fixed';st.left='0';st.top='0';st.right='0';st.bottom='0';"
         L"st.pointerEvents='none';st.zIndex='2147483647';"
         L"st.background='repeating-linear-gradient(0deg,rgba(0,0,0,0) 0px,rgba(0,0,0,0) 2px,rgba(0,0,0,.09) 2px,rgba(0,0,0,.09) 3px)';"
         L"(document.documentElement||document.body).appendChild(d);}"
         L"if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',ap);else ap();"
         L"}catch(e){}})();";
    return s;
}

static void ApplyScan(CefRefPtr<CefBrowser> b) {
    if (b) b->GetMainFrame()->ExecuteJavaScript(ScanScript(g.scan), b->GetMainFrame()->GetURL(), 0);
}

static void RunUserscripts(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> frame, userscripts::RunAt phase) {
    if (!b || !frame || !frame->IsMain()) return;
    std::wstring url = frame->GetURL().ToWString();
    if (url.empty()) return;
    for (const auto& s : userscripts::ScriptsForUrl(url, phase, APP_VERSION)) {
        frame->ExecuteJavaScript(s.js, s.label, 0);
    }
}

static std::wstring ErrorText(int code) {
    switch (code) {
    case -105: case -137: return L"HOST NOT FOUND - check the address";
    case -106: return L"NO NETWORK - you appear to be offline";
    case -7: case -118: return L"TIMEOUT - the server took too long";
    case -102: case -109: return L"CONNECTION REFUSED - server unreachable";
    case -101: return L"CONNECTION RESET";
    case -21: return L"NETWORK CHANGED - try again";
    default:
        if (code <= -200 && code > -300) return L"INSECURE CONNECTION - certificate rejected";
        return L"SIGNAL INTERRUPTED (code " + std::to_wstring(code) + L")";
    }
}

// scheme, lower-case host (without brackets or user name) and port of an address; port 0 = none given
static void SplitUrl(const std::wstring& u, std::wstring& scheme, std::wstring& host, int& port) {
    scheme.clear(); host.clear(); port = 0;
    size_t s = u.find(L"://");
    if (s == std::wstring::npos) return;
    scheme = u.substr(0, s);
    for (auto& c : scheme) c = (wchar_t)towlower(c);
    std::wstring a = u.substr(s + 3);
    a = a.substr(0, a.find_first_of(L"/?#"));
    size_t at = a.rfind(L'@');
    if (at != std::wstring::npos) a = a.substr(at + 1);
    if (!a.empty() && a[0] == L'[') {
        size_t e = a.find(L']');
        if (e == std::wstring::npos) return;
        host = a.substr(1, e - 1);
        if (e + 1 < a.size() && a[e + 1] == L':') port = _wtoi(a.c_str() + e + 2);
    } else {
        size_t c = a.rfind(L':');
        if (c != std::wstring::npos) { port = _wtoi(a.c_str() + c + 1); a.resize(c); }
        host = a;
    }
    for (auto& c : host) c = (wchar_t)towlower(c);
}

// "host:port" with the default port filled in: what a certificate exception is kept under
static std::wstring HostPortKey(const std::wstring& u) {
    std::wstring sc, h; int p;
    SplitUrl(u, sc, h, p);
    if (!p) p = sc == L"http" ? 80 : 443;
    return h + L":" + std::to_wstring(p);
}
static std::wstring HostOnly(const std::wstring& u) { std::wstring sc, h; int p; SplitUrl(u, sc, h, p); return h; }

// Addresses HTTPS-only mode leaves alone: this computer, the local network and anything that is
// not a registered name (address literals and single-word names cannot have a certificate).
static bool HttpsExempt(const std::wstring& h) {
    if (h.empty() || h.find(L'.') == std::wstring::npos || h.find(L':') != std::wstring::npos) return true;
    auto ends = [&](const wchar_t* e) { size_t n = wcslen(e); return h.size() >= n && h.compare(h.size() - n, n, e) == 0; };
    if (ends(L".localhost") || ends(L".local") || ends(L".lan") || ends(L".internal") || ends(L".home.arpa")) return true;
    bool digits = true;
    for (wchar_t c : h) if (!((c >= L'0' && c <= L'9') || c == L'.')) digits = false;
    return digits;   // an IPv4 address
}

static bool IsCertError(int code) { return code <= -200 && code >= -219; }

// ---------------------------------------------------------------------------
// Saved passwords
//
// %LOCALAPPDATA%\NeonNet\passwords.dat. Every secret in it is encrypted with Windows DPAPI (the key
// belongs to the signed-in Windows account; no other user or computer can open the file): a line per
// saved login holds the site and user name as one encrypted blob and the password as another, and a
// line per "never save for this site" holds the site as a blob. In memory only the password stays
// encrypted: it is decrypted for the moment it is filled into a page, and a page never receives it
// before the user has clicked a saved user name. Incognito windows neither save nor fill.

static const BYTE PW_ENTROPY[] = "NeonNet passwords v1";

static bool DpProtect(const std::string& plain, std::string& blob) {
    DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() }, ent{ (DWORD)(sizeof(PW_ENTROPY) - 1), (BYTE*)PW_ENTROPY }, out{};
    if (!CryptProtectData(&in, L"NeonNet", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    blob.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
static bool DpUnprotect(const std::string& blob, std::string& plain) {
    DATA_BLOB in{ (DWORD)blob.size(), (BYTE*)blob.data() }, ent{ (DWORD)(sizeof(PW_ENTROPY) - 1), (BYTE*)PW_ENTROPY }, out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain.assign((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
static std::string B64(const std::string& s) { return CefBase64Encode(s.data(), s.size()).ToString(); }
static bool UnB64(const std::string& s, std::string& out) {
    CefRefPtr<CefBinaryValue> b = CefBase64Decode(s);
    if (!b) return false;
    out.assign(b->GetSize(), '\0');
    if (!out.empty()) b->GetData(&out[0], out.size(), 0);
    return true;
}

struct PwEntry { int id = 0; std::wstring origin, user; std::string passBlob; };
static std::mutex g_pwMu;                 // the settings page reads the list on the IO thread
static std::vector<PwEntry> g_pw;
static std::set<std::wstring> g_pwNever;
static int g_pwNextId = 1;

// scheme://host[:port], the unit a saved login belongs to
static std::wstring OriginOf(const std::wstring& u) {
    std::wstring sc, h; int p;
    SplitUrl(u, sc, h, p);
    if (sc.empty() || h.empty()) return L"";
    std::wstring o = sc + L"://" + (h.find(L':') != std::wstring::npos ? L"[" + h + L"]" : h);
    if (p && !((sc == L"http" && p == 80) || (sc == L"https" && p == 443))) o += L":" + std::to_wstring(p);
    return o;
}

static std::wstring PwPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\passwords.dat"; }

static void PwSaveLocked() {
    std::string text = "NEONNET-PASSWORDS 1\n";
    for (auto& e : g_pw) {
        std::string hdr;
        if (DpProtect(Utf8(e.origin) + "\x1f" + Utf8(e.user), hdr)) text += "E\t" + B64(hdr) + "\t" + B64(e.passBlob) + "\n";
    }
    for (auto& o : g_pwNever) {
        std::string b;
        if (DpProtect(Utf8(o), b)) text += "N\t" + B64(b) + "\n";
    }
    std::wstring path = PwPath(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

static void PwLoad() {
    std::lock_guard<std::mutex> lk(g_pwMu);
    g_pw.clear(); g_pwNever.clear();
    HANDLE h = CreateFileW(PwPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string data;
    if (size > 0 && size < 16 * 1024 * 1024) {
        data.resize(size);
        if (!ReadFile(h, &data[0], size, &got, nullptr) || got != size) data.clear();
    }
    CloseHandle(h);
    size_t pos = data.find('\n');
    if (pos == std::string::npos || data.compare(0, pos, "NEONNET-PASSWORDS 1") != 0) return;
    for (++pos; pos < data.size();) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(pos, e - pos);
        pos = e + 1;
        std::vector<std::string> f;
        for (size_t s = 0;;) { size_t t = line.find('\t', s); f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s)); if (t == std::string::npos) break; s = t + 1; }
        std::string hdr, plain;
        if (f.size() == 3 && f[0] == "E" && UnB64(f[1], hdr) && DpUnprotect(hdr, plain)) {
            size_t sep = plain.find('\x1f');
            PwEntry en;
            en.id = g_pwNextId++;
            en.origin = Wide(plain.substr(0, sep));
            en.user = sep == std::string::npos ? L"" : Wide(plain.substr(sep + 1));
            if (UnB64(f[2], en.passBlob) && !en.origin.empty()) g_pw.push_back(std::move(en));
        } else if (f.size() == 2 && f[0] == "N" && UnB64(f[1], hdr) && DpUnprotect(hdr, plain)) {
            g_pwNever.insert(Wide(plain));
        }
        if (g_pw.size() > 5000) break;
    }
}

static std::vector<std::wstring> PwUsers(const std::wstring& origin) {
    std::lock_guard<std::mutex> lk(g_pwMu);
    std::vector<std::wstring> v;
    for (auto& e : g_pw) if (e.origin == origin) v.push_back(e.user);
    return v;
}
static bool PwFind(const std::wstring& origin, const std::wstring& user, std::wstring& pass) {
    std::lock_guard<std::mutex> lk(g_pwMu);
    for (auto& e : g_pw) {
        if (e.origin != origin || e.user != user) continue;
        std::string plain;
        if (!DpUnprotect(e.passBlob, plain)) return false;
        pass = Wide(plain);
        SecureZeroMemory(&plain[0], plain.size());
        return true;
    }
    return false;
}
static bool PwHas(const std::wstring& origin, const std::wstring& user) {
    std::lock_guard<std::mutex> lk(g_pwMu);
    for (auto& e : g_pw) if (e.origin == origin && e.user == user) return true;
    return false;
}
static bool PwNeverHas(const std::wstring& origin) { std::lock_guard<std::mutex> lk(g_pwMu); return g_pwNever.count(origin) != 0; }
static bool PwSame(const std::wstring& origin, const std::wstring& user, const std::wstring& pass) {
    std::wstring cur;
    if (!PwFind(origin, user, cur)) return false;
    bool same = cur == pass;
    SecureZeroMemory(&cur[0], cur.size() * sizeof(wchar_t));
    return same;
}
static void PwStore(const std::wstring& origin, const std::wstring& user, const std::wstring& pass) {
    std::string blob;
    if (!DpProtect(Utf8(pass), blob)) return;
    std::lock_guard<std::mutex> lk(g_pwMu);
    for (auto& e : g_pw) if (e.origin == origin && e.user == user) { e.passBlob = blob; PwSaveLocked(); return; }
    if (g_pw.size() >= 5000) return;
    PwEntry en; en.id = g_pwNextId++; en.origin = origin; en.user = user; en.passBlob = blob;
    g_pw.push_back(std::move(en));
    PwSaveLocked();
}
static void PwDelete(int id) {
    std::lock_guard<std::mutex> lk(g_pwMu);
    g_pw.erase(std::remove_if(g_pw.begin(), g_pw.end(), [&](const PwEntry& e) { return e.id == id; }), g_pw.end());
    PwSaveLocked();
}
static void PwNeverAdd(const std::wstring& origin) { std::lock_guard<std::mutex> lk(g_pwMu); g_pwNever.insert(origin); PwSaveLocked(); }
static void PwNeverRemove(const std::wstring& origin) { std::lock_guard<std::mutex> lk(g_pwMu); g_pwNever.erase(origin); PwSaveLocked(); }
static void PwClear() { std::lock_guard<std::mutex> lk(g_pwMu); g_pw.clear(); PwSaveLocked(); }
static size_t PwCount() { std::lock_guard<std::mutex> lk(g_pwMu); return g_pw.size(); }
static std::string PwJsonList();   // below, next to JsonUtf8

// ---------------------------------------------------------------------------
// Site permissions
// What a site may do (camera, location, ...) is remembered per origin in sitepermissions.txt and can be
// changed from the lock icon in the address bar or from neonnet://permissions. 1 = allow, 2 = block,
// no entry = ask. An incognito window keeps its answers in memory only.

struct PermDef {
    const wchar_t* key; const wchar_t* label; const wchar_t* ask;
    uint32_t bit;     // CEF_PERMISSION_TYPE_* flags that mean this permission (permission prompt)
    uint32_t media;   // CEF_MEDIA_PERMISSION_* flags that mean it (camera and microphone requests)
    bool common;      // always listed in the site panel, even when nothing is stored
};
static const PermDef PERM_DEFS[] = {
    { L"location", L"Location", L"know your location", CEF_PERMISSION_TYPE_GEOLOCATION, 0, true },
    { L"camera", L"Camera", L"use your camera", CEF_PERMISSION_TYPE_CAMERA_STREAM, CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE, true },
    { L"microphone", L"Microphone", L"use your microphone", CEF_PERMISSION_TYPE_MIC_STREAM, CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE, true },
    { L"notifications", L"Notifications", L"show notifications", CEF_PERMISSION_TYPE_NOTIFICATIONS, 0, true },
    { L"clipboard", L"Clipboard", L"see what you copy to the clipboard", CEF_PERMISSION_TYPE_CLIPBOARD, 0, true },
    { L"sensors", L"Motion sensors", L"use your motion sensors", CEF_PERMISSION_TYPE_SENSORS, 0, true },
    { L"screen", L"Screen sharing", L"see and record your screen", 0, CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE | CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE, false },
    { L"camera-control", L"Camera movement and zoom", L"move and zoom your camera", CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, 0, false },
    { L"midi", L"MIDI devices", L"use your MIDI devices", CEF_PERMISSION_TYPE_MIDI_SYSEX, 0, false },
    { L"keyboard-lock", L"Keyboard lock", L"take over your keyboard", CEF_PERMISSION_TYPE_KEYBOARD_LOCK, 0, false },
    { L"pointer-lock", L"Mouse pointer lock", L"hide and capture your mouse pointer", CEF_PERMISSION_TYPE_POINTER_LOCK, 0, false },
    { L"multiple-downloads", L"Several downloads", L"download several files", CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, 0, false },
    { L"local-fonts", L"Installed fonts", L"see the fonts installed on this computer", CEF_PERMISSION_TYPE_LOCAL_FONTS, 0, false },
    { L"idle-detection", L"Idle detection", L"know when you are idle", CEF_PERMISSION_TYPE_IDLE_DETECTION, 0, false },
    { L"window-management", L"Window management", L"manage windows on all your screens", CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, 0, false },
    { L"file-system", L"File editing", L"edit files on this computer", CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, 0, false },
    { L"embedded-storage", L"Cookies when embedded", L"use its cookies and site data while embedded in another site", CEF_PERMISSION_TYPE_STORAGE_ACCESS | CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS, 0, false },
    { L"protected-media", L"Protected content", L"identify this device for protected content", CEF_PERMISSION_TYPE_PROTECTED_MEDIA_IDENTIFIER, 0, false },
    { L"protocol-handler", L"Link handling", L"open links of its own kind", CEF_PERMISSION_TYPE_REGISTER_PROTOCOL_HANDLER, 0, false },
    { L"local-network", L"Local network", L"reach devices on your network", CEF_PERMISSION_TYPE_LOCAL_NETWORK | CEF_PERMISSION_TYPE_LOOPBACK_NETWORK, 0, false },
    { L"other", L"Other device features", L"use another feature of this device", 0, 0, false },
};

// The permissions a request is for, by key (a request can ask for several at once).
static std::vector<std::wstring> PermKeys(bool media, uint32_t p) {
    std::vector<std::wstring> v;
    uint32_t known = 0;
    for (const auto& d : PERM_DEFS) { uint32_t m = media ? d.media : d.bit; known |= m; if (m && (p & m)) v.push_back(d.key); }
    if ((p & ~known) || v.empty()) v.push_back(L"other");
    return v;
}
static bool PermKeyValid(const std::wstring& k) { for (const auto& d : PERM_DEFS) if (k == d.key) return true; return false; }

static std::mutex g_spMu;                                            // the IO thread lists them for neonnet://permissions
static std::map<std::wstring, std::map<std::wstring, int>> g_sp;     // origin -> key -> 1 allow, 2 block

static std::wstring SpPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\sitepermissions.txt"; }
static bool SpOriginValid(const std::wstring& o) {
    if (o.size() < 9 || o.size() > 300) return false;
    if (o.rfind(L"https://", 0) != 0 && o.rfind(L"http://", 0) != 0) return false;
    for (wchar_t c : o) if (c < 0x21) return false;
    return OriginOf(o) == o;
}

static void SpSaveLocked() {
    if (g.incognito) return;
    std::string text = "NEONNET-SITEPERMS 1\n";
    for (const auto& s : g_sp) for (const auto& e : s.second) text += Utf8(s.first) + "\t" + Utf8(e.first) + "\t" + std::to_string(e.second) + "\n";
    std::wstring path = SpPath(), tmp = path + L".tmp";
    if (g_sp.empty()) { DeleteFileW(path.c_str()); return; }
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

static void SpLoad() {
    std::lock_guard<std::mutex> lk(g_spMu);
    g_sp.clear();
    if (g.incognito) return;
    HANDLE h = CreateFileW(SpPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string data;
    if (size > 0 && size < 4 * 1024 * 1024) {
        data.resize(size);
        if (!ReadFile(h, &data[0], size, &got, nullptr) || got != size) data.clear();
    }
    CloseHandle(h);
    size_t pos = data.find('\n');
    if (pos == std::string::npos || data.compare(0, pos, "NEONNET-SITEPERMS 1") != 0) return;
    size_t count = 0;
    for (++pos; pos < data.size() && count < 20000;) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(pos, e - pos);
        pos = e + 1;
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::wstring origin = Wide(line.substr(0, t1)), key = Wide(line.substr(t1 + 1, t2 - t1 - 1));
        int v = atoi(line.c_str() + t2 + 1);
        if ((v == 1 || v == 2) && SpOriginValid(origin) && PermKeyValid(key)) { g_sp[origin][key] = v; count++; }
    }
}

static int SpGet(const std::wstring& origin, const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_spMu);
    auto s = g_sp.find(origin);
    if (s == g_sp.end()) return 0;
    auto e = s->second.find(key);
    return e == s->second.end() ? 0 : e->second;
}
// v: 1 allow, 2 block, 0 ask again (the entry is removed)
static void SpSet(const std::wstring& origin, const std::wstring& key, int v) {
    std::lock_guard<std::mutex> lk(g_spMu);
    if (v == 0) {
        auto s = g_sp.find(origin);
        if (s != g_sp.end()) { s->second.erase(key); if (s->second.empty()) g_sp.erase(s); }
    } else if (g_sp.size() < 5000 || g_sp.count(origin)) g_sp[origin][key] = v;
    SpSaveLocked();
}
static void SpReset(const std::wstring& origin) { std::lock_guard<std::mutex> lk(g_spMu); g_sp.erase(origin); SpSaveLocked(); }
static void SpClear() { std::lock_guard<std::mutex> lk(g_spMu); g_sp.clear(); SpSaveLocked(); }

// [{"k":"camera","l":"Camera","v":1},...] for one origin: the common permissions, then any other one that is set
static std::wstring SpPermsJson(const std::wstring& origin) {
    std::lock_guard<std::mutex> lk(g_spMu);
    const std::map<std::wstring, int>* m = nullptr;
    auto s = g_sp.find(origin);
    if (s != g_sp.end()) m = &s->second;
    std::wstring j = L"[";
    bool first = true;
    for (const auto& d : PERM_DEFS) {
        int v = 0;
        if (m) { auto e = m->find(d.key); if (e != m->end()) v = e->second; }
        if (!d.common && !v) continue;
        if (!first) j += L",";
        first = false;
        j += L"{\"k\":" + JsonStr(d.key) + L",\"l\":" + JsonStr(d.label) + L",\"v\":" + std::to_wstring(v) + L"}";
    }
    return j + L"]";
}

// What the lock icon panel shows for the page at this address (null: no panel for it)
static std::wstring SiteJson(const std::wstring& url) {
    std::wstring sc, h; int p;
    SplitUrl(url, sc, h, p);
    if (h.empty() || (sc != L"https" && sc != L"http")) return L"null";
    bool internal = h == L"neonnet.ui";
    std::wstring origin = OriginOf(url);
    std::wstring j = L"{\"origin\":" + JsonStr(origin) + L",\"host\":" + JsonStr(h) + L",\"kind\":" +
                     JsonStr(internal ? L"internal" : sc == L"https" ? L"secure" : L"insecure") +
                     L",\"warn\":" + (!internal && sc == L"https" && g.certAllow.count(HostPortKey(url)) ? L"true" : L"false");
    if (!internal) j += L",\"perms\":" + SpPermsJson(origin);
    return j + L"}";
}

// Smooth scrolling is done by SMOOTH_JS in the page; the browser process tells it whether it is on and how
// far and how long to scroll (message nn.cfg), when the page starts and when the setting changes.
static void SendSmooth(CefRefPtr<CefFrame> f) {
    if (!f) return;
    CefRefPtr<CefProcessMessage> m = CefProcessMessage::Create("nn.cfg");
    CefRefPtr<CefListValue> l = m->GetArgumentList();
    l->SetInt(0, g.smooth ? 1 : 0); l->SetInt(1, g.smoothStep); l->SetInt(2, g.smoothTime);
    f->SendProcessMessage(PID_RENDERER, m);
}
static void BroadcastSmooth() {
    for (auto& t : g.tabs) if (t->browser) SendSmooth(t->browser->GetMainFrame());
}



// The warning page (certificate errors, HTTPS-only mode). It is a NeonNet page, so its buttons can
// talk to the app; "kind" is "cert" or "https", "url" the address the user asked for.
static std::wstring WarningPageUrl(const wchar_t* kind, const std::wstring& url, int code) {
    return std::wstring(L"https://neonnet.ui/warning.html?kind=") + kind + L"&code=" + std::to_wstring(code) +
           L"&url=" + CefURIEncode(url, false).ToWString();
}

static std::wstring ErrorPageUrl(const std::wstring& url, int code) {
    std::wstring h =
        L"<!doctype html><html><head><meta charset='utf-8'><title>SIGNAL LOST</title><style>"
        L"html,body{margin:0;height:100%;background:#12062c;color:#e8d8ff;font:14px 'Cascadia Mono',Consolas,monospace}"
        L"body{display:flex;align-items:center;justify-content:center}"
        L".b{border:1px solid #ff2e97;padding:28px 34px;max-width:640px;background:#0a0420;"
        L"clip-path:polygon(0 0,calc(100% - 16px) 0,100% 16px,100% 100%,16px 100%,0 calc(100% - 16px))}"
        L"h1{margin:0 0 6px;font:italic 900 40px Impact,sans-serif;letter-spacing:3px;color:#00e5ff;text-shadow:3px 3px 0 #ff2e97}"
        L".y{color:#ffd600}.u{color:#8a6fc0;word-break:break-all;margin:14px 0 22px}"
        L"button{font:bold 14px 'Cascadia Mono',Consolas,monospace;background:#ff2e97;color:#12062c;border:0;padding:9px 22px;cursor:pointer}"
        L"button:hover{background:#00e5ff}"
        L"body::after{content:'';position:fixed;inset:0;pointer-events:none;background:repeating-linear-gradient(0deg,transparent 0 2px,rgba(0,0,0,.18) 2px 3px)}"
        L"</style></head><body><div class='b'><h1>SIGNAL LOST</h1><div class='y'>&gt; ";
    h += HtmlEsc(ErrorText(code));
    h += L"</div><div class='u' id='u' data-url=\"" + HtmlEsc(url) + L"\">" + HtmlEsc(url) + L"</div>";
    h += L"<button id='r'>RETRY</button></div><script>"
         L"document.getElementById('r').onclick=()=>{location.href=document.getElementById('u').dataset.url};"
         L"</script></body></html>";
    std::string utf8 = Utf8(h);
    return L"data:text/html;charset=utf-8;base64," + CefBase64Encode(utf8.data(), utf8.size()).ToWString();
}

// ---------------------------------------------------------------------------
// Fullscreen

static void SetFullscreen(bool on) {
    if (on == g.fullscreen) return;
    if (on) {
        g.fsPrev.length = sizeof(g.fsPrev);
        GetWindowPlacement(g.hwnd, &g.fsPrev);
        if (IsZoomed(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
        g.fullscreen = true;
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        RECT m = mi.rcMonitor;
        SetWindowPos(g.hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        g.fullscreen = false;
        SetWindowPlacement(g.hwnd, &g.fsPrev);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    Layout();
}
static void UpdateFullscreen() { SetFullscreen(g.fsUser || g.fsElement); }

// ---------------------------------------------------------------------------
// Tabs

static void ExecCmd(int cmd);
static void CreateTab(const std::wstring& url, bool activate, int at = -1);
static void CloseTab(int id);
static CefRefPtr<CefClient> MainClient();

// The find bar belongs to the tab it was opened in: it closes when another tab becomes
// active. Callers update the layout afterwards.
static void HideFind() {
    if (!g.findOpen) return;
    if (Tab* a = Active()) if (a->browser) a->browser->GetHost()->StopFinding(true);
    g.findOpen = false;
}

static void RestoreTab(Tab* t);   // below: gives an unloaded tab its page back

static void SelectTab(int id, bool focus) {
    Tab* sel = FindTab(id);
    if (!sel) return;
    if (id != g.active) HideFind();
    g.active = id;
    if (FindWs(sel->wsId)) {
        g.activeWs = sel->wsId;
        FindWs(sel->wsId)->activeTabId = id;
    }
    sel->lastActive = GetTickCount64();
    if (sel->discarded) RestoreTab(sel);
    if (g.fsElement) { g.fsElement = false; UpdateFullscreen(); }
    g.hoverText.clear();
    Layout();
    SendState();
    InvalidateStatus();
    ApplyBackgroundLimit();
    ApplyNetworkLimit();
    if (focus) FocusPage();
}

static CefWindowInfo ChildInfo(const RECT& r) {
    CefWindowInfo wi;
    wi.SetAsChild(g.hwnd, CefRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
    return wi;
}

static CefBrowserSettings BrowserSettings() {
    CefBrowserSettings s;
    s.background_color = CEF_BG;
    // Our browsers are Chrome style, which brings Chromium's own bubbles. NeonNet shows the
    // link under the mouse and the zoom level in its status bar, so these would be shown twice.
    s.chrome_status_bubble = STATE_DISABLED;
    s.chrome_zoom_bubble = STATE_DISABLED;
    return s;
}

// at: position in the tab strip, or -1 for right after the active tab.
static void CreateTab(const std::wstring& url, bool activate, int at) {
    auto tab = std::make_unique<Tab>();
    int id = tab->id = g.nextId++;
    tab->url = url;
    tab->wsId = g.activeWs;
    tab->lastActive = GetTickCount64();
    int cur = IndexOf(g.active);
    int pos = at >= 0 ? std::min(at, (int)g.tabs.size()) : (cur < 0 ? (int)g.tabs.size() : cur + 1);
    g.tabs.insert(g.tabs.begin() + pos, std::move(tab));
    KeepPinsFirst();   // a new tab opened next to a pinned one must not end up among the pinned tabs
    if (activate) { HideFind(); g.active = id; }

    g.creatingTab = id;
    CefRefPtr<CefBrowser> b = CefBrowserHost::CreateBrowserSync(ChildInfo(ContentRect()), MainClient(),
        url.empty() ? NEWTAB : url, BrowserSettings(), nullptr, g.reqCtx);
    g.creatingTab = 0;
    Tab* t = FindTab(id);
    if (!b) { if (t) CloseTab(id); return; }
    if (t && !t->browser) t->browser = b;
    Layout();
    SendState();
    if (activate) FocusPage();
}

// A tab unloaded by the performance panel gets a browser again, showing the address it had.
static void RestoreTab(Tab* t) {
    if (!t || !t->discarded || t->browser) return;
    t->discarded = false; t->loading = true; t->pid = 0; t->netEnabled = false; t->netLimitKB = -1;
    std::wstring url = DisplayUrl(t);
    g.creatingTab = t->id;
    CefRefPtr<CefBrowser> b = CefBrowserHost::CreateBrowserSync(ChildInfo(ContentRect()), MainClient(),
        url.empty() ? NEWTAB : url, BrowserSettings(), nullptr, g.reqCtx);
    g.creatingTab = 0;
    if (b && !t->browser) t->browser = b;
}

static void CloseTab(int id) {
    int i = IndexOf(id);
    if (i < 0) return;
    int ws = g.tabs[i]->wsId;
    int wsPos = 0;
    for (int k = 0; k < i; k++) if (g.tabs[k]->wsId == ws) wsPos++;
    std::unique_ptr<Tab> t = std::move(g.tabs[i]);
    g.tabs.erase(g.tabs.begin() + i);
    if (!g.closing) {
        std::wstring u = DisplayUrl(t.get());
        if (!u.empty() && u.rfind(NEWTAB, 0) != 0 && u.rfind(L"data:", 0) != 0) {
            g.closed.push_back({ u, i, t->pinned, t->wsId });
            if (g.closed.size() > 25) g.closed.erase(g.closed.begin());
        }
    }
    if (t->browser) {
        HWND w = t->browser->GetHost()->GetWindowHandle();
        if (w) ShowWindow(w, SW_HIDE);
        t->browser->GetHost()->CloseBrowser(false);
    }
    std::vector<int> sameWs;
    for (auto& o : g.tabs) if (o->wsId == g.activeWs) sameWs.push_back(o->id);
    if (sameWs.empty()) { if (g.active == id) g.active = 0; CreateTab(NEWTAB, true); return; }
    if (g.active == id) SelectTab(sameWs[std::min(wsPos, (int)sameWs.size() - 1)], true);
    else SendState();
}

// Pin or unpin a tab. A pinned tab goes to the end of the pinned tabs; an unpinned one to the
// first place after them.
static void SetPinned(int id, bool pin) {
    int from = IndexOf(id);
    if (from < 0 || g.tabs[from]->pinned == pin) return;
    std::unique_ptr<Tab> t = std::move(g.tabs[from]);
    g.tabs.erase(g.tabs.begin() + from);
    t->pinned = pin;
    g.tabs.insert(g.tabs.begin() + PinnedCount(), std::move(t));
    SendState();
}

// Drag and drop in the tab strip. A tab stays inside its own group (pinned vs unpinned)
// within the active workspace.
static void MoveTab(int id, int visTo) {
    int from = IndexOf(id);
    if (from < 0) return;
    int ws = g.tabs[from]->wsId;
    std::vector<int> vis;
    int visPins = 0, visFrom = -1;
    for (int k = 0; k < (int)g.tabs.size(); k++) {
        if (g.tabs[k]->wsId != ws) continue;
        if (k == from) visFrom = (int)vis.size();
        if (g.tabs[k]->pinned) visPins++;
        vis.push_back(k);
    }
    if (visFrom < 0 || vis.empty()) { SendState(); return; }
    bool p = g.tabs[from]->pinned;
    visTo = std::max(p ? 0 : visPins, std::min(visTo, p ? visPins - 1 : (int)vis.size() - 1));
    int to = vis[visTo];
    if (to != from) {
        std::unique_ptr<Tab> t = std::move(g.tabs[from]);
        g.tabs.erase(g.tabs.begin() + from);
        g.tabs.insert(g.tabs.begin() + to, std::move(t));
    }
    SendState();   // also when nothing moved: the strip then snaps back to the real order
}

static void SwitchWorkspace(int wsId) {
    Workspace* target = FindWs(wsId);
    if (!target) return;
    if (Workspace* cur = FindWs(g.activeWs)) cur->activeTabId = g.active;
    g.activeWs = wsId;
    SaveWorkspaces();
    int pick = 0;
    if (target->activeTabId) {
        if (Tab* t = FindTab(target->activeTabId)) if (t->wsId == wsId) pick = t->id;
    }
    if (!pick) {
        for (auto& t : g.tabs) if (t->wsId == wsId) { pick = t->id; break; }
    }
    if (pick) SelectTab(pick, true);
    else CreateTab(NEWTAB, true);
}

static int CreateWorkspace(std::wstring name, std::wstring icon, std::wstring color, int moveTabId = 0) {
    EnsureDefaultWorkspaces();
    if (g.workspaces.size() >= 12) return 0;
    name = SanitizeWsField(std::move(name), 24);
    if (name.empty()) name = L"WS " + std::to_wstring(g.nextWsId);
    icon = SanitizeWsField(std::move(icon), 4);
    if (icon.empty()) icon = L"\u25C6";
    color = SanitizeWsField(std::move(color), 16);
    if (color.empty() || color[0] != L'#') color = L"#00e5ff";

    Workspace w;
    w.id = g.nextWsId++;
    w.name = std::move(name);
    w.icon = std::move(icon);
    w.color = std::move(color);
    g.workspaces.push_back(w);
    if (moveTabId > 0) {
        if (Tab* t = FindTab(moveTabId)) t->wsId = w.id;
    }
    SaveWorkspaces();
    SwitchWorkspace(w.id);
    return w.id;
}

static void UpdateWorkspace(int wsId, std::wstring name, std::wstring icon, std::wstring color) {
    Workspace* w = FindWs(wsId);
    if (!w) return;
    name = SanitizeWsField(std::move(name), 24);
    icon = SanitizeWsField(std::move(icon), 4);
    color = SanitizeWsField(std::move(color), 16);
    if (!name.empty()) w->name = std::move(name);
    if (!icon.empty()) w->icon = std::move(icon);
    if (!color.empty() && color[0] == L'#') w->color = std::move(color);
    SaveWorkspaces();
    SendState();
}

static void MoveTabToWorkspace(int tabId, int wsId) {
    Tab* t = FindTab(tabId);
    if (!t || !FindWs(wsId) || t->wsId == wsId) return;
    int oldWs = t->wsId;
    t->wsId = wsId;
    if (Workspace* dst = FindWs(wsId)) dst->activeTabId = tabId;
    if (tabId == g.active) {
        int nextInOld = 0;
        for (auto& o : g.tabs) if (o->wsId == oldWs) { nextInOld = o->id; break; }
        if (nextInOld) SelectTab(nextInOld, true);
        else CreateTab(NEWTAB, true);
    } else {
        SendState();
    }
}

static void DeleteWorkspace(int wsId) {
    if (g.workspaces.size() <= 1) return;
    int idx = -1;
    for (int i = 0; i < (int)g.workspaces.size(); i++) if (g.workspaces[i].id == wsId) { idx = i; break; }
    if (idx < 0) return;
    int fallback = g.workspaces[idx == 0 ? 1 : 0].id;
    for (auto& t : g.tabs) if (t->wsId == wsId) t->wsId = fallback;
    g.workspaces.erase(g.workspaces.begin() + idx);
    if (g.activeWs == wsId) SwitchWorkspace(fallback);
    else { SaveWorkspaces(); SendState(); }
}

// ---------------------------------------------------------------------------
// Shutdown: close every browser, quit the CEF loop once the last one is gone.

static size_t OpenBrowsers();
static void MaybeFinishClose() {
    if (g.closing && OpenBrowsers() == 0 && g.devtools == 0) {
        // Post it: destroying the window from inside OnBeforeClose crashes libcef while the loop unwinds.
        static bool posted = false; if (!posted) { posted = true; PostMessageW(g.hwnd, WM_APP_FINISH, 0, 0); }
    }
}

// ---------------------------------------------------------------------------
// CEF client (shared by the chrome UI browser and all tab browsers)

// The tools' own "Dock side" menu (the three dots) reports a choice to the embedder with
// DevToolsHost.sendMessageToEmbedder({method:'setPreference', params:['currentDockState','"right"']}). This
// wrapper passes it on as a console message that DevToolsClient::OnConsoleMessage turns into the new position.
static const char DEVTOOLS_DOCK_JS[] =
    "(function(){var d=window.DevToolsHost;if(!d||d.__nnDock)return;d.__nnDock=1;var o=d.sendMessageToEmbedder;"
    "d.sendMessageToEmbedder=function(m){try{var j=JSON.parse(m);"
    // the tools' own X button asks the embedder to close "the window"; here that would close the whole browser
    "if(j.method==='closeWindow'){console.log('__NN_CLOSE__');return;}"
    "if(j.method==='setPreference'&&j.params&&j.params[0]==='currentDockState')console.log('__NN_DOCK__'+JSON.parse(j.params[1]));"
    // where the tools leave room for the page (CSS pixels from their own top left corner)
    "else if(j.method==='setInspectedPageBounds'&&j.params&&j.params[0]){var b=j.params[0];console.log('__NN_BOUNDS__'+b.x+','+b.y+','+b.width+','+b.height);}}catch(e){}"
    "return o.apply(this,arguments);};"
    // The tools remember their own last dock side, which is not necessarily the one NeonNet wants (or knows):
    // tell them where they are meant to be once they are ready.
    "var want=window.__nnWant,n=0;if(want)import('devtools://devtools/bundled/ui/legacy/legacy.js').then(function(m){"
    "(function t(){try{var c=m.DockController.DockController.instance();if(c.dockSide()!==want)c.setDockSide(want);setTimeout(function(){console.log('__NN_READY__');},300);}"
    "catch(e){if(++n<40)setTimeout(t,250);else console.log('__NN_READY__');}})();"
    "}).catch(function(){console.log('__NN_READY__');});"
    "})();";

class DevToolsClient : public CefClient, public CefLifeSpanHandler, public CefKeyboardHandler, public CefLoadHandler, public CefDisplayHandler {
public:
    explicit DevToolsClient(int tabId) : tabId_(tabId) {}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (!frame->IsMain()) return;
        // Some of the tools' features (the device toolbar) exist only when their address says "can_dock=true",
        // which an embedded window does not get: load the same address once more with it. The script below runs
        // after that load.
        std::string url = frame->GetURL().ToString();
        if (url.rfind("devtools://", 0) == 0 && url.find("can_dock=") == std::string::npos) {
            frame->LoadURL(url + (url.find('?') == std::string::npos ? "?" : "&") + "can_dock=true");
            return;
        }
        Tab* t = FindTab(tabId_);
        static const char* SIDES[] = { "bottom", "right", "left", "undocked" };
        std::string js = std::string("window.__nnWant='") + SIDES[t ? t->devDock : 3] + "';" + DEVTOOLS_DOCK_JS;
        frame->ExecuteJavaScript(js, "devtools://neonnet-dock", 0);
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message, const CefString&, int) override {
        std::wstring m = message.ToWString();
        if (m.rfind(L"__NN_BOUNDS__", 0) == 0) {
            wchar_t* p = &m[13];
            double v[4];
            for (int i = 0; i < 4; i++) { wchar_t* e; v[i] = wcstod(p, &e); if (e == p) return true; p = *e ? e + 1 : e; }
            Tab* t = FindTab(tabId_);
            if (!t || !t->dev || g.closing) return true;
            double sc = g.dpi / 96.0;
            RECT h = { (LONG)lround(v[0] * sc), (LONG)lround(v[1] * sc), (LONG)lround((v[0] + v[2]) * sc), (LONG)lround((v[1] + v[3]) * sc) };
            if (h.right <= h.left || h.bottom <= h.top) return true;
            if (!t->devHoleSet || memcmp(&h, &t->devHole, sizeof(RECT)) != 0) { t->devHole = h; t->devHoleSet = true; Layout(); }
            return true;
        }
        if (m == L"__NN_READY__") { ready_ = true; return true; }
        if (m == L"__NN_CLOSE__") {   // the X button of the tools: close only them
            Tab* t = FindTab(tabId_);
            if (t && t->browser && t->dev && !g.closing) { t->browser->GetHost()->CloseDevTools(); FocusPage(); }
            return true;
        }
        if (m.rfind(L"__NN_DOCK__", 0) != 0) return false;
        if (!ready_) return true;   // what the tools report while they start up is their own old choice, not a new one
        std::wstring side = m.substr(11);
        int nd = side == L"bottom" ? 0 : side == L"right" ? 1 : side == L"left" ? 2 : side == L"undocked" ? 3 : -1;
        Tab* t = FindTab(tabId_);
        if (nd < 0 || !t || !t->dev || nd == t->devDock || g.closing) return true;
        DWORD v = (DWORD)nd;
        RegSetBin(L"DevToolsDock", REG_DWORD, &v, sizeof(v));   // the next tools open where this choice put them
        g.devDock = nd;
        if ((nd == 3) != (t->devDock == 3)) {   // to or from a window of its own: reopen, the kind of host window changes
            t->devReopen = true;
            t->browser->GetHost()->CloseDevTools();
        } else { t->devDock = nd; Layout(); }
        return true;
    }
    // The developer tools are a Chromium page: without this, Chromium would answer window shortcuts itself
    // (Ctrl+Shift+N would open a Chromium window). Only the shortcuts the tools do not use themselves are taken;
    // the rest (Ctrl+F, Ctrl+P, Ctrl+Shift+P, Ctrl+Shift+M, zoom ...) stay with the tools.
    bool OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& e, CefEventHandle, bool*) override {
        if (e.type != KEYEVENT_RAWKEYDOWN) return false;
        bool ctrl = (e.modifiers & EVENTFLAG_CONTROL_DOWN) != 0, shift = (e.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
        bool alt = (e.modifiers & EVENTFLAG_ALT_DOWN) != 0, repeat = (e.modifiers & EVENTFLAG_IS_REPEAT) != 0;
        int vk = e.windows_key_code, cmd = 0;
        if (repeat) return false;
        if (ctrl && !alt) {
            if (vk == 'N' && shift) cmd = CMD_INCOGNITO;
            else if (vk == 'T') cmd = shift ? CMD_REOPEN : CMD_NEWTAB;
            else if (vk == 'H' && !shift) cmd = CMD_HISTORY;
            else if (vk == 'J' && !shift) cmd = CMD_DOWNLOADS;
            else if (vk == 'E' && shift) cmd = CMD_EXTENSIONS;
            else if (vk == VK_OEM_COMMA) cmd = CMD_SETTINGS;
            else if (vk == VK_TAB) cmd = shift ? CMD_PREVTAB : CMD_NEXTTAB;
        } else if (!ctrl && !alt && !shift) {
            if (vk == VK_F1) cmd = CMD_HELP;
            else if (vk == VK_F11) cmd = CMD_FULLSCREEN;
            else if (vk == VK_F12) cmd = CMD_DEVTOOLS;   // closes the tools
        }
        if (!cmd) return false;
        ExecCmd(cmd);
        return true;
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        g.devtools++;
        if (Tab* t = FindTab(tabId_)) { t->dev = b; Layout(); }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        g.devtools--;
        if (Tab* t = FindTab(tabId_)) {
            t->dev = nullptr; t->devHoleSet = false;
            if (!g.closing) Layout();
            if (t->devReopen) { t->devReopen = false; if (!g.closing) PostMessageW(g.hwnd, WM_APP_DEVOPEN, (WPARAM)t->id, 0); }
        }
        MaybeFinishClose();
    }
private:
    int tabId_;
    bool ready_ = false;   // the tools have been told where they are: from now on a dock message is the user's choice
    IMPLEMENT_REFCOUNTING(DevToolsClient);
};

// Opens (or, when already open, shows and points at an element of) the developer tools of a tab,
// docked in the NeonNet window below the page.
static void OpenDevTools(Tab* t, CefPoint at) {
    if (!t || !t->browser) return;
    if (!t->dev) {
        t->devDock = g.devDock;
        // Tell the tools where they are, so their Dock side menu marks the right choice (they would start as "undocked")
        if (CefRefPtr<CefRequestContext> ctx = t->browser->GetHost()->GetRequestContext()) {
            CefRefPtr<CefValue> cur = ctx->GetPreference("devtools.preferences");
            CefRefPtr<CefDictionaryValue> d = cur && cur->GetType() == VTYPE_DICTIONARY ? cur->GetDictionary()->Copy(false) : CefDictionaryValue::Create();
            static const char* SIDES[] = { "\"bottom\"", "\"right\"", "\"left\"", "\"undocked\"" };
            d->SetString("currentDockState", SIDES[t->devDock]);
            CefRefPtr<CefValue> nv = CefValue::Create();
            nv->SetDictionary(d);
            CefString err;
            ctx->SetPreference("devtools.preferences", nv, err);
        }
    }
    CefWindowInfo wi;
    if (t->devDock == 3) wi.SetAsPopup(nullptr, "NeonNet DevTools");
    else {
        RECT r = ContentRect();   // the tools fill the whole page area and leave a hole for the page (see Layout)
        wi.SetAsChild(g.hwnd, CefRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
    }
    t->browser->GetHost()->ShowDevTools(wi, new DevToolsClient(t->id), CefBrowserSettings(), at);
}

// One row of a NeonNet-themed menu (the ≡ menu and the right-click menu; see RunMenu below).
struct MenuItem {
    int cmd = 0;              // 0 = separator
    std::wstring text, keys;
    bool disabled = false, checked = false;
};

// A right-click menu taken over from Chromium: its rows, and the callback that gets the choice.
struct CtxMenu {
    CefRefPtr<CefRunContextMenuCallback> callback;
    std::vector<MenuItem> items;
    POINT at{};
};

// "&Back" -> "Back" (the "&" marks a keyboard mnemonic; "&&" is a literal ampersand), and
// "Back\tAlt+Left" -> "Back".
static std::wstring CleanMenuLabel(const std::wstring& in) {
    std::wstring s = in.substr(0, in.find(L'\t')), o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'&') { if (i + 1 < s.size() && s[i + 1] == L'&') { o += L'&'; i++; } continue; }
        o += s[i];
    }
    if (o.size() > 56) { o.resize(55); o += L"\x2026"; }
    return o;
}

static std::wstring AcceleratorText(CefRefPtr<CefMenuModel> m, size_t i) {
    int key = 0; bool shift = false, ctrl = false, alt = false;
    if (!m->GetAcceleratorAt(i, key, shift, ctrl, alt) || !key) return {};
    std::wstring k;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) k = std::wstring(1, (wchar_t)key);
    else if (key >= VK_F1 && key <= VK_F12) k = L"F" + std::to_wstring(key - VK_F1 + 1);
    else switch (key) {
        case VK_LEFT: k = L"Left"; break;
        case VK_RIGHT: k = L"Right"; break;
        case VK_DELETE: k = L"Del"; break;
        case VK_RETURN: k = L"Enter"; break;
        default: return {};
    }
    return std::wstring(ctrl ? L"Ctrl+" : L"") + (alt ? L"Alt+" : L"") + (shift ? L"Shift+" : L"") + k;
}

// Chromium's own items that open a new tab or window would open a stray Chromium window (its
// commands know nothing about NeonNet's tabs), or lead to Chromium pages and Google services.
// NeonNet adds its own versions of the useful ones (see OnBeforeContextMenu); these are hidden.
static bool ReplacedByNeonNet(const std::wstring& label) {
    std::wstring l = label;
    for (auto& c : l) c = (wchar_t)towlower(c);
    static const wchar_t* exact[] = {
        L"open link in new tab", L"open link in new window", L"open link in incognito window",
        L"open image in new tab", L"open video in new tab", L"open audio in new tab", L"view page source", L"inspect",
        L"language settings", L"cast\x2026", L"cast...",
    };
    for (auto* e : exact) if (l == e) return true;
    return l.rfind(L"search ", 0) == 0 || l.rfind(L"create qr", 0) == 0 || l.rfind(L"send to your", 0) == 0 ||
           l.rfind(L"copy link to highlight", 0) == 0;
}

// Copy a Chromium menu model into rows.
static void AppendMenuModel(CefRefPtr<CefMenuModel> m, std::vector<MenuItem>& out, int depth) {
    for (size_t i = 0; i < m->GetCount(); i++) {
        if (!m->IsVisibleAt(i)) continue;
        cef_menu_item_type_t type = m->GetTypeAt(i);
        if (type == MENUITEMTYPE_SEPARATOR) { out.push_back(MenuItem()); continue; }
        MenuItem it;
        it.text = CleanMenuLabel(m->GetLabelAt(i).ToWString());
        if (it.text.empty()) continue;
        // Submenus (spell check languages, writing direction) are not offered: they are long, and
        // some lead to Chromium settings pages.
        if (type == MENUITEMTYPE_SUBMENU) continue;
        it.cmd = m->GetCommandIdAt(i);
        if (!it.cmd) continue;
        bool ours = it.cmd >= MENU_OPEN_LINK_TAB && it.cmd <= MENU_SEARCH_SELECTION;   // added in OnBeforeContextMenu
        if (!ours && ReplacedByNeonNet(it.text)) continue;
        it.disabled = !m->IsEnabledAt(i);
        it.checked = (type == MENUITEMTYPE_CHECK || type == MENUITEMTYPE_RADIO) && m->IsCheckedAt(i);
        it.keys = AcceleratorText(m, i);
        out.push_back(std::move(it));
    }
    if (depth == 0) {   // no separator first, last or twice in a row
        std::vector<MenuItem> t;
        for (auto& r : out) { if (!r.cmd && (t.empty() || !t.back().cmd)) continue; t.push_back(std::move(r)); }
        while (!t.empty() && !t.back().cmd) t.pop_back();
        out = std::move(t);
    }
}

// Containers and codecs this build of Chromium cannot play (H.264, AAC and friends are left out
// of the build for licensing reasons). Chromium turns a direct link to one of these into a download.
static bool IsUnplayableMime(std::wstring m) {
    for (auto& c : m) c = (wchar_t)towlower(c);
    size_t semi = m.find(L';');
    if (semi != std::wstring::npos) m.resize(semi);
    while (!m.empty() && m.back() == L' ') m.pop_back();
    static const wchar_t* list[] = {
        L"video/mp4", L"video/x-m4v", L"video/quicktime", L"video/3gpp", L"video/3gpp2", L"video/mp2t",
        L"video/x-matroska", L"video/x-msvideo", L"video/avi", L"video/x-ms-wmv", L"video/x-flv", L"video/mpeg",
        L"audio/mp4", L"audio/x-m4a", L"audio/m4a", L"audio/aac", L"audio/x-aac", L"audio/aacp", L"audio/x-ms-wma",
    };
    for (auto* s : list) if (m == s) return true;
    return false;
}

// A server that sends Content-Disposition: attachment wants the file saved, not played.
static bool IsAttachment(const std::wstring& cd) {
    size_t i = cd.find_first_not_of(L" \t");
    return i != std::wstring::npos && _wcsnicmp(cd.c_str() + i, L"attachment", 10) == 0;
}

static std::vector<CefRefPtr<CefBrowser>> g_all;   // every live browser we own (for shutdown)
static size_t OpenBrowsers() { return g_all.size(); }

// ---------------------------------------------------------------------------
// Tracker and ad blocking
//
// ui/blocklist.txt (embedded) lists advertising and tracking domains. A request to a listed domain
// (or one of its subdomains) is cancelled when the page that makes it belongs to another site, and
// never for the page itself: main frame navigations and a site's own requests are left alone.

static std::string g_blockArena;                     // single contiguous backing buffer for g_blockSorted
static std::vector<std::string_view> g_blockSorted;   // sorted unique domains (filled once before g_blocklistReady)
static std::atomic<bool> g_blocklistReady{ false };    // set after the fill: the IO thread reads the list only then

static void LoadBlocklist() {
    if (g_blocklistReady.load()) return;
    if (!LoadUi(IDR_BLOCKLIST, g_blockArena)) return;
    for (auto& c : g_blockArena) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    g_blockSorted.reserve(90000);
    size_t pos = 0;
    const size_t len = g_blockArena.size();
    while (pos < len) {
        size_t e = g_blockArena.find('\n', pos);
        if (e == std::string::npos) e = len;
        size_t s = pos;
        pos = e + 1;
        while (e > s && (g_blockArena[e - 1] == '\r' || g_blockArena[e - 1] == ' ' || g_blockArena[e - 1] == '\t')) e--;
        if (e <= s || g_blockArena[s] == '#') continue;
        g_blockSorted.emplace_back(g_blockArena.data() + s, e - s);
    }
    std::sort(g_blockSorted.begin(), g_blockSorted.end());
    g_blockSorted.erase(std::unique(g_blockSorted.begin(), g_blockSorted.end()), g_blockSorted.end());
    g_blockSorted.shrink_to_fit();
    g_blocklistReady = true;
}

// The part of a host name a site is known by: example.com, or example.co.uk for the common two-part endings.
static std::string_view SiteOf(std::string_view host) {
    size_t d1 = host.rfind('.');
    if (d1 == std::string_view::npos || d1 == 0) return host;
    size_t d2 = host.rfind('.', d1 - 1);
    if (d2 == std::string_view::npos) return host;
    static const std::string_view second[] = { "co", "com", "net", "org", "gov", "edu", "ac", "or", "go" };
    std::string_view tld = host.substr(d1 + 1);
    std::string_view sld = host.substr(d2 + 1, d1 - d2 - 1);
    bool two = tld.size() == 2;
    bool isSecond = false;
    for (auto x : second) if (sld == x) { isSecond = true; break; }
    if (two && isSecond) {
        if (d2 == 0) return host;
        size_t d3 = host.rfind('.', d2 - 1);
        return d3 == std::string_view::npos ? host : host.substr(d3 + 1);
    }
    return host.substr(d2 + 1);
}

static bool IsBlockedHost(std::string_view host) {
    for (size_t s = 0; s < host.size();) {
        size_t d = host.find('.', s);
        if (d == std::string_view::npos) break;   // never the bare ending (com, uk ...)
        std::string_view sub = host.substr(s);
        if (std::binary_search(g_blockSorted.begin(), g_blockSorted.end(), sub)) return true;
        s = d + 1;
    }
    return false;
}

class BlockHandler : public CefResourceRequestHandler {
public:
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, CefRefPtr<CefCallback>) override {
        g.blockedCount++;
        return RV_CANCEL;
    }
    IMPLEMENT_REFCOUNTING(BlockHandler);
};

// Loads an address in a tab, from a task (not from inside the browser's own callback).
class NavTask : public CefTask {
public:
    NavTask(int tabId, std::wstring url) : id_(tabId), url_(std::move(url)) {}
    void Execute() override {
        Tab* t = FindTab(id_);
        if (t && t->browser) t->browser->GetMainFrame()->LoadURL(url_);
    }
private:
    int id_; std::wstring url_;
    IMPLEMENT_REFCOUNTING(NavTask);
};

class Client : public CefClient, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler,
               public CefKeyboardHandler, public CefDownloadHandler, public CefContextMenuHandler,
               public CefRequestHandler, public CefFindHandler, public CefJSDialogHandler, public CefPermissionHandler {
public:
    CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }

    // --- dialogs, permission prompts and sign-in: shown by ShowDlgReq() in the NeonNet theme ---
    static bool QueueDialog(DlgReq* r) {
        if (!g.hwnd || g.closing || !PostMessageW(g.hwnd, WM_APP_DIALOG, 0, (LPARAM)r)) { delete r; return false; }
        return true;
    }
    bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url, JSDialogType type, const CefString& message_text,
                    const CefString& default_prompt_text, CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message) override {
        if (IsUiBrowser(browser)) return false;
        auto* r = new DlgReq;
        r->kind = type == JSDIALOGTYPE_ALERT ? DlgReq::ALERT : type == JSDIALOGTYPE_CONFIRM ? DlgReq::CONFIRM : DlgReq::PROMPT;
        if (Tab* t = TabOf(browser)) r->tabId = t->id;
        r->origin = origin_url.ToWString(); r->message = message_text.ToWString(); r->defText = default_prompt_text.ToWString();
        r->js = callback;
        if (!QueueDialog(r)) callback->Continue(false, CefString());
        return true;
    }
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser, const CefString&, bool is_reload, CefRefPtr<CefJSDialogCallback> callback) override {
        if (IsUiBrowser(browser)) return false;
        auto* r = new DlgReq;
        r->kind = DlgReq::UNLOAD; r->isReload = is_reload; r->js = callback;
        if (Tab* t = TabOf(browser)) { r->tabId = t->id; r->origin = t->url; }
        if (!QueueDialog(r)) callback->Continue(true, CefString());
        return true;
    }
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t, const CefString& requesting_origin, uint32_t requested_permissions,
                                CefRefPtr<CefPermissionPromptCallback> callback) override {
        // NeonNet's own pages never ask: the toolbar loading a site's icon from a local address would
        // otherwise bring up Chromium's "access other apps on this device" bubble
        if (IsUiBrowser(browser)) { callback->Continue(CEF_PERMISSION_RESULT_DENY); return true; }
        auto* r = new DlgReq;
        r->kind = DlgReq::PERM; r->origin = requesting_origin.ToWString(); r->perms = requested_permissions; r->perm = callback;
        if (Tab* t = TabOf(browser)) r->tabId = t->id;
        if (!QueueDialog(r)) callback->Continue(CEF_PERMISSION_RESULT_DISMISS);
        return true;
    }
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, const CefString& requesting_origin,
                                        uint32_t requested_permissions, CefRefPtr<CefMediaAccessCallback> callback) override {
        if (IsUiBrowser(browser)) { callback->Cancel(); return true; }
        auto* r = new DlgReq;
        r->kind = DlgReq::MEDIA; r->origin = requesting_origin.ToWString(); r->perms = requested_permissions; r->media = callback;
        if (Tab* t = TabOf(browser)) r->tabId = t->id;
        if (!QueueDialog(r)) callback->Cancel();
        return true;
    }

    // --- life span ---
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString& target_url,
                       const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&,
                       CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>&, bool*) override {
        if (IsUiBrowser(browser) || g.closing) return true;   // the chrome UI never opens popups
        // Keep the popup (so window.opener works for logins) but host it as a new tab.
        auto tab = std::make_unique<Tab>();
        int id = tab->id = g.nextId++;
        tab->url = target_url.ToWString();
        int at = IndexOf(g.active);
        if (at < 0) g.tabs.push_back(std::move(tab));
        else g.tabs.insert(g.tabs.begin() + at + 1, std::move(tab));
        KeepPinsFirst();
        HideFind();
        g.active = id;
        g.pendingPopups.push_back(id);
        windowInfo = ChildInfo(ContentRect());
        client = this;
        settings = BrowserSettings();
        SendState();
        return false;
    }

    void OnBeforePopupAborted(CefRefPtr<CefBrowser>, int) override {
        if (g.pendingPopups.empty()) return;
        int id = g.pendingPopups.back();
        g.pendingPopups.pop_back();
        int i = IndexOf(id);
        if (i >= 0) { g.tabs.erase(g.tabs.begin() + i); if (!g.tabs.empty() && g.active == id) SelectTab(g.tabs.front()->id, true); }
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        g_all.push_back(browser);
        if (g.creatingChrome) { g.chrome = browser; g.chromeId = browser->GetIdentifier(); return; }
        if (g.creatingPanel) { g.panel = browser; g.panelId = browser->GetIdentifier(); return; }
        Tab* t = nullptr;
        if (g.creatingTab) t = FindTab(g.creatingTab);
        else if (!g.pendingPopups.empty()) { t = FindTab(g.pendingPopups.front()); g.pendingPopups.pop_front(); }
        if (t) { t->browser = browser; t->netEnabled = false; t->netLimitKB = -1; if (t->muted) browser->GetHost()->SetAudioMuted(true); ApplyNetworkLimit(); }
        else browser->GetHost()->CloseBrowser(true);   // unknown window: refuse it
        Layout();
        SendState();
        if (t && t->id == g.active) FocusPage();
    }

    bool DoClose(CefRefPtr<CefBrowser>) override { return true; }   // we manage our own top-level window

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        int bid = browser->GetIdentifier();
        g_all.erase(std::remove_if(g_all.begin(), g_all.end(),
            [bid](const CefRefPtr<CefBrowser>& b) { return b->GetIdentifier() == bid; }), g_all.end());
        if (IsChrome(browser)) { g.chrome = nullptr; g.chromeId = 0; }
        else if (IsPanel(browser)) { g.panel = nullptr; g.panelId = 0; }
        else if (Tab* t = TabOf(browser)) {
            t->browser = nullptr;
            if (t->discarding) {
                // unloaded on purpose (UnloadTab): the tab stays, without a browser, until it is selected
                t->discarding = false; t->discarded = true; t->pid = 0; t->loading = false; t->memBytes = 0; t->cpu = 0; t->netEnabled = false; t->netLimitKB = -1;
                if (!g.closing) SendState();
            } else if (!g.closing) {
                CloseTab(t->id);   // closed from inside the page (window.close())
            }
        }
        MaybeFinishClose();
    }

    // --- find ---
    void OnFindResult(CefRefPtr<CefBrowser> browser, int, int count, const CefRect&, int activeMatchOrdinal, bool) override {
        Tab* t = TabOf(browser);
        if (t && t->id == g.active && g.findOpen) FindResult(count, std::max(0, activeMatchOrdinal));
    }

    // --- display ---
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
        if (Tab* t = TabOf(browser)) {
            t->title = title.ToWString();
            if (t->title.rfind(L"data:text/html", 0) == 0) t->title = L"SIGNAL LOST";
            if (t->histId) { store::SetTitle(t->histId, t->title); ScheduleSave(); }
            SendState();
        }
    }
    // The page's own icon. Only plain http(s) addresses are passed on to the tab strip.
    void OnFaviconURLChange(CefRefPtr<CefBrowser> browser, const std::vector<CefString>& icon_urls) override {
        Tab* t = TabOf(browser);
        if (!t) return;
        std::wstring icon;
        for (const auto& u : icon_urls) {
            std::wstring w = u.ToWString();
            if (w.size() < 2000 && (w.rfind(L"https://", 0) == 0 || w.rfind(L"http://", 0) == 0)) { icon = w; break; }
        }
        if (icon != t->icon) { t->icon = icon; if (g.favicons) SendState(); }
    }
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (!frame->IsMain()) return;
        if (Tab* t = TabOf(browser)) {
            // a page on another site brings its own icon; until it arrives the tab shows none
            auto site = [](const std::wstring& u) { size_t s = u.find(L"://"); size_t e = s == std::wstring::npos ? s : u.find(L'/', s + 3); return u.substr(0, e); };
            if (site(url.ToWString()) != site(t->url)) t->icon.clear();
            t->url = url.ToWString();
            if (t->errorPending) t->errorPending = false;
            else t->errorUrl.clear();
            if (t->errorUrl.empty()) {
                t->histId = store::Visit(t->histId, t->url);
                if (t->histId) ScheduleSave();
            }
            SendState();
        }
    }
    void OnStatusMessage(CefRefPtr<CefBrowser> browser, const CefString& value) override {
        Tab* t = TabOf(browser);
        if (t && t->id == g.active) { g.hoverText = value.ToWString(); InvalidateStatus(); }
    }
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override {
        Tab* t = TabOf(browser);
        if (!t || t->id != g.active) return;
        g.fsElement = fullscreen;
        UpdateFullscreen();
    }

    // --- load ---
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward) override {
        if (Tab* t = TabOf(browser)) {
            t->loading = isLoading;
            t->canBack = canGoBack;
            t->canFwd = canGoForward;
            if (!isLoading) ApplyScan(browser);
            SendState();
            if (t->id == g.active) InvalidateStatus();
        } else if (IsChrome(browser) && !isLoading) {
            SendState();
        } else if (IsPanel(browser) && !isLoading) {
            PushPerf();
        }
    }
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType) override {
        if (frame->IsMain()) {
            if (Tab* t = TabOf(browser)) {
                t->dirty = false;
                ApplyScan(browser);
                RunUserscripts(browser, frame, userscripts::RUN_START);
            }
        }   // a new page: nothing typed yet
    }
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override {
        if (!frame->IsMain() || httpStatusCode == 0) return;
        if (TabOf(browser)) {
            RunUserscripts(browser, frame, userscripts::RUN_END);
            RunUserscripts(browser, frame, userscripts::RUN_IDLE);
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                     const CefString&, const CefString& failedUrl) override {
        if (!frame->IsMain() || errorCode == ERR_ABORTED) return;
        Tab* t = TabOf(browser);
        if (!t) return;
        std::wstring url = failedUrl.ToWString();
        if (url.rfind(UI_HOST, 0) == 0 || url.rfind(L"data:", 0) == 0) return;
        std::wstring shown = url, page;
        if (!t->upgradedTo.empty() && HostPortKey(url) == HostPortKey(t->upgradedTo)) {
            // HTTPS-only mode: the secure address did not work, so ask before using plain http
            shown = t->upgradedFrom;
            g.warned.insert(L"http|" + HostOnly(shown));
            page = WarningPageUrl(L"https", shown, (int)errorCode);
            t->upgradedFrom.clear(); t->upgradedTo.clear();
        } else if (IsCertError((int)errorCode)) {
            g.warned.insert(L"cert|" + HostPortKey(url));
            page = WarningPageUrl(L"cert", url, (int)errorCode);
        } else page = ErrorPageUrl(url, (int)errorCode);
        t->errorUrl = shown;
        t->errorPending = true;
        store::Forget(t->histId, shown);   // failed loads stay out of the history
        t->histId = 0;
        frame->LoadURL(page);
    }

    // --- certificates and HTTPS-only mode ---
    bool OnCertificateError(CefRefPtr<CefBrowser> browser, cef_errorcode_t, const CefString& request_url, CefRefPtr<CefSSLInfo>,
                            CefRefPtr<CefCallback> callback) override {
        // Not allowed: the load is cancelled and OnLoadError() shows the warning page. Allowed: the user
        // chose "proceed" for this host earlier in this run.
        if (IsUiBrowser(browser) || !callback) return false;
        if (g.certAllow.count(HostPortKey(request_url.ToWString()))) { callback->Continue(); return true; }
        return false;
    }
    // Runs on the IO thread: only the atomics and the read-only block list are used here.
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
                                                                   bool is_navigation, bool, const CefString&, bool&) override {
        if (!g.blockAds.load() || is_navigation || !browser || !g_blocklistReady.load()) return nullptr;
        int bid = browser->GetIdentifier();
        if (bid == g.chromeId.load() || bid == g.panelId.load()) return nullptr;
        std::string ru = request->GetURL().ToString();
        size_t hs = 0;
        if (ru.compare(0, 8, "https://") == 0) hs = 8;
        else if (ru.compare(0, 7, "http://") == 0) hs = 7;
        else return nullptr;
        size_t he = ru.find_first_of("/?#", hs);
        std::string reqHost = ru.substr(hs, he == std::string::npos ? std::string::npos : he - hs);
        size_t at = reqHost.rfind('@'); if (at != std::string::npos) reqHost.erase(0, at + 1);
        size_t col = reqHost.find(':'); if (col != std::string::npos) reqHost.resize(col);
        for (auto& c : reqHost) c = (char)tolower((unsigned char)c);
        if (reqHost.empty() || !IsBlockedHost(reqHost)) return nullptr;
        // the site that is open, from its main frame
        std::string page = browser->GetMainFrame() ? browser->GetMainFrame()->GetURL().ToString() : "";
        if (page.compare(0, 18, "https://neonnet.ui") == 0) return nullptr;
        size_t ps = 0;
        if (page.compare(0, 8, "https://") == 0) ps = 8;
        else if (page.compare(0, 7, "http://") == 0) ps = 7;
        if (ps) {
            size_t pe = page.find_first_of("/?#", ps);
            std::string pHost = page.substr(ps, pe == std::string::npos ? std::string::npos : pe - ps);
            size_t pat = pHost.rfind('@'); if (pat != std::string::npos) pHost.erase(0, pat + 1);
            size_t pcol = pHost.find(':'); if (pcol != std::string::npos) pHost.resize(pcol);
            for (auto& c : pHost) c = (char)tolower((unsigned char)c);
            if (!pHost.empty() && SiteOf(pHost) == SiteOf(reqHost)) return nullptr;   // first party
        }
        static CefRefPtr<BlockHandler> handler = new BlockHandler();
        return handler;
    }
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool) override {
        if (!frame->IsMain() || IsUiBrowser(browser)) return false;
        Tab* t = TabOf(browser);
        std::wstring url = request->GetURL().ToWString();
        if (t && !t->upgradedTo.empty() && url != t->upgradedTo) { t->upgradedFrom.clear(); t->upgradedTo.clear(); }   // another page
        if (!g.httpsOnly || !t || url.rfind(L"http://", 0) != 0 || request->GetMethod() != "GET") return false;
        std::wstring host = HostOnly(url);
        if (HttpsExempt(host) || g.httpAllow.count(host)) return false;
        // upgrade: try the same address over https, and keep the original for the warning page
        t->upgradedFrom = url;
        t->upgradedTo = L"https://" + url.substr(7);
        CefPostTask(TID_UI, new NavTask(t->id, t->upgradedTo));
        return true;
    }

    // --- request ---
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString&) override {
        crash::LogSubprocess(L"renderer", (int)status, error_code);
        if (Tab* t = TabOf(browser)) {
            std::wstring url = DisplayUrl(t);
            t->errorUrl = url;
            t->errorPending = true;
            browser->GetMainFrame()->LoadURL(ErrorPageUrl(url, error_code ? error_code : -1));
        }
    }

    // --- keyboard ---
    bool OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& e, CefEventHandle, bool*) override {
        if (e.type != KEYEVENT_RAWKEYDOWN) return false;
        bool ctrl = (e.modifiers & EVENTFLAG_CONTROL_DOWN) != 0, shift = (e.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
        bool alt = (e.modifiers & EVENTFLAG_ALT_DOWN) != 0, repeat = (e.modifiers & EVENTFLAG_IS_REPEAT) != 0;
        int vk = e.windows_key_code, cmd = 0;
        if (ctrl && !alt) {
            switch (vk) {
            case 'T': if (!repeat) cmd = shift ? CMD_REOPEN : CMD_NEWTAB; break;
            case 'F': cmd = CMD_FIND; break;
            case 'G': cmd = shift ? CMD_FINDPREV : CMD_FINDNEXT; break;
            case 'D': if (!repeat) cmd = CMD_BOOKMARK; break;
            case 'B': if (shift && !repeat) cmd = CMD_BOOKMARKS_BAR; break;
            case 'N': if (shift && !repeat) cmd = CMD_INCOGNITO; break;
            case 'M': if (shift && !repeat) cmd = CMD_PERFORMANCE; break;
            case 'H': if (!repeat) cmd = CMD_HISTORY; break;
            case 'J': if (!repeat) cmd = CMD_DOWNLOADS; break;
            case VK_OEM_COMMA: if (!repeat) cmd = CMD_SETTINGS; break;
            case 'O': if (shift && !repeat) cmd = CMD_BOOKMARKS; break;
            case 'E': if (shift && !repeat) cmd = CMD_EXTENSIONS; break;
            case 'W': case VK_F4: if (!repeat) cmd = CMD_CLOSETAB; break;
            case 'L': cmd = CMD_FOCUSURL; break;
            case 'R': cmd = CMD_RELOAD; break;
            case 'P': if (!repeat) cmd = CMD_PRINT; break;
            case VK_TAB: cmd = shift ? CMD_PREVTAB : CMD_NEXTTAB; break;
            case VK_OEM_PLUS: case VK_ADD: cmd = CMD_ZOOMIN; break;
            case VK_OEM_MINUS: case VK_SUBTRACT: cmd = CMD_ZOOMOUT; break;
            case '0': case VK_NUMPAD0: cmd = CMD_ZOOMRESET; break;
            default:
                if (vk >= '1' && vk <= '9') {
                    std::vector<int> vis;
                    for (auto& t : g.tabs) if (t->wsId == g.activeWs) vis.push_back(t->id);
                    if (!vis.empty()) {
                        size_t i = (vk == '9') ? vis.size() - 1 : (size_t)(vk - '1');
                        if (i < vis.size()) { SelectTab(vis[i], true); return true; }
                    }
                }
            }
        } else if (alt && !ctrl) {
            if (vk == VK_LEFT) cmd = CMD_BACK;
            else if (vk == VK_RIGHT) cmd = CMD_FWD;
            else if (vk == VK_HOME) cmd = CMD_HOME;
            else if (vk >= '1' && vk <= '9' && !repeat) {
                size_t wi = (size_t)(vk - '1');
                if (wi < g.workspaces.size()) { SwitchWorkspace(g.workspaces[wi].id); return true; }
            }
        } else if (!ctrl && !alt) {
            if (vk == VK_F1 && !repeat) cmd = CMD_HELP;
            else if (vk == VK_F5) cmd = CMD_RELOAD;
            else if (vk == VK_F3) cmd = shift ? CMD_FINDPREV : CMD_FINDNEXT;
            else if (vk == VK_F11 && !repeat) cmd = CMD_FULLSCREEN;
            else if (vk == VK_F12 && !repeat) cmd = CMD_DEVTOOLS;
            else if (vk == VK_F6) cmd = CMD_FOCUSURL;
            else if (vk == VK_ESCAPE && g.fsUser) cmd = CMD_FULLSCREEN;
        }
        if (!cmd) return false;
        ExecCmd(cmd);
        return true;
    }

    // --- downloads ---
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, const CefString& suggested_name,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override {
        std::wstring url = item->GetURL().ToWString();
        std::wstring mime = item->GetMimeType().ToWString();
        bool forPlayer = !g.pendingPlayerUrl.empty() && url == g.pendingPlayerUrl;   // asked for in the context menu
        if (forPlayer) g.pendingPlayerUrl.clear();
        bool forSave = !g.pendingSaveUrl.empty() && url == g.pendingSaveUrl;   // already asked
        if (forSave) g.pendingSaveUrl.clear();
        if (!forPlayer && !forSave && !g.closing && IsUnplayableMime(mime) && !IsAttachment(item->GetContentDisposition().ToWString())) {
            // A video NeonNet cannot play: ask what to do (from the main window loop, not from this callback).
            auto* ask = new MediaAsk{ callback, item->GetId(), url, suggested_name.ToWString(), mime };
            if (PostMessageW(g.hwnd, WM_APP_MEDIA, 0, (LPARAM)ask)) return true;
            delete ask;
        }
        if (forPlayer) {
            std::wstring tmp = TempMediaPath(suggested_name.ToWString(), mime);
            if (!tmp.empty()) {
                g.tempDl[item->GetId()] = tmp;
                callback->Continue(tmp, false);
                return true;
            }
        }
        // listed on the downloads page from the moment the save dialog opens
        if (store::DownloadUpdate(item->GetId(), item->GetURL().ToWString(), L"", suggested_name.ToWString(), 0, 0, 0, store::DL_ACTIVE))
            ScheduleSave();
        std::wstring target = RegGetDword(L"AskDownload", 1) ? std::wstring() : DownloadTarget(suggested_name.ToWString());
        if (target.empty()) callback->Continue("", true);   // ask where to save
        else callback->Continue(target, false);              // settings: save to the Downloads folder
        return true;
    }
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback> cb) override {
        std::wstring path = item->GetFullPath().ToWString();
        std::wstring name = path;
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        auto temp = g.tempDl.find(item->GetId());
        if (temp != g.tempDl.end()) {
            // Copy for the system player: not listed as a download, opened when complete, removed later.
            std::wstring tmpPath = temp->second;
            if (item->IsComplete()) {
                g.tempDl.erase(temp); g.download.clear();
                OpenTempMedia(path.empty() ? tmpPath : path);
            } else if (item->IsCanceled() || item->IsInterrupted()) {
                g.tempDl.erase(temp); g.download.clear();
                DeleteFileW(tmpPath.c_str());
            } else {
                g.download = L"PREPARING " + name + L" " + std::to_wstring(std::max(0, item->GetPercentComplete())) + L"%";
            }
            InvalidateStatus();
            return;
        }
        int state = item->IsComplete() ? store::DL_COMPLETE : item->IsCanceled() ? store::DL_CANCELED :
                    item->IsInterrupted() ? store::DL_INTERRUPTED : store::DL_ACTIVE;
        unsigned id = item->GetId();
        if (state == store::DL_ACTIVE) { if (cb) g.dlCallbacks[id] = cb; }
        else g.dlCallbacks.erase(id);
        // the state is saved when it changes, not on every progress report (that would keep
        // postponing the save timer)
        if (store::DownloadUpdate(id, item->GetURL().ToWString(), path, L"", item->GetTotalBytes(), item->GetReceivedBytes(),
                                  item->GetCurrentSpeed(), state))
            ScheduleSave();
        if (item->IsComplete()) g.download = L"DOWNLOADED " + name;
        else if (item->IsCanceled() || item->IsInterrupted()) g.download.clear();
        else if (item->IsInProgress() && !name.empty())
            g.download = L"DOWNLOADING " + name + L" " + std::to_wstring(std::max(0, item->GetPercentComplete())) + L"%";
        InvalidateStatus();
    }

    // --- context menu ---
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        if (IsUiBrowser(browser)) {
            if (!(params->GetTypeFlags() & CM_TYPEFLAG_EDITABLE)) model->Clear();
            return;
        }
        // Items are inserted at the top, last group first. Chromium's own items for these actions are
        // hidden when the menu is shown (ReplacedByNeonNet), because they would open stray windows.
        int flags = params->GetTypeFlags();
        std::wstring link = params->GetLinkUrl().ToWString();
        bool httpLink = link.rfind(L"http://", 0) == 0 || link.rfind(L"https://", 0) == 0;
        cef_context_menu_media_type_t media = params->GetMediaType();
        std::wstring src = params->GetSourceUrl().ToWString();
        bool httpSrc = src.rfind(L"http://", 0) == 0 || src.rfind(L"https://", 0) == 0;
        if ((media == CM_MEDIATYPE_VIDEO || media == CM_MEDIATYPE_AUDIO) && httpSrc) {
            model->InsertItemAt(0, MENU_OPEN_PLAYER, "Open in system player");
            model->InsertSeparatorAt(1);
        }
        if (media == CM_MEDIATYPE_IMAGE && httpSrc) {
            model->InsertItemAt(0, MENU_OPEN_IMAGE_TAB, "Open image in new tab");
            model->InsertSeparatorAt(1);
        }
        if (!link.empty()) {
            int at = 0;
            model->InsertItemAt(at++, MENU_OPEN_LINK_TAB, "Open link in new tab");
            if (!g.incognito && httpLink) model->InsertItemAt(at++, MENU_OPEN_LINK_INCOGNITO, "Open link in incognito window");
            model->InsertSeparatorAt(at);
        }
        std::wstring sel = params->GetSelectionText().ToWString();
        if ((flags & CM_TYPEFLAG_SELECTION) && !sel.empty()) {
            // one line, shortened, for the menu text
            for (auto& c : sel) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (sel.size() > 28) { sel.resize(27); sel += L"\x2026"; }
            model->InsertItemAt(0, MENU_SEARCH_SELECTION, L"Search " + std::wstring(CurrentEngine().name) + L" for \"" + sel + L"\"");
            model->InsertSeparatorAt(1);
        }
        if (model->GetCount() > 0) model->AddSeparator();
        if ((flags & CM_TYPEFLAG_PAGE) && !(flags & (CM_TYPEFLAG_LINK | CM_TYPEFLAG_MEDIA | CM_TYPEFLAG_SELECTION | CM_TYPEFLAG_EDITABLE)))
            model->AddItem(MENU_VIEW_SOURCE, "View page source");
        model->AddItem(MENU_INSPECT, "Inspect");
    }
    // Show the right-click menu in the NeonNet theme instead of Chromium's native menu. The rows are
    // copied now (the model must not be kept); the menu itself runs from the window procedure.
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
                        CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback) override {
        if (g.closing || !g.hwnd) return false;
        auto* m = new CtxMenu;
        m->callback = callback;
        AppendMenuModel(model, m->items, 0);
        if (m->items.empty()) { delete m; callback->Cancel(); return true; }   // e.g. the toolbar outside a text box
        GetCursorPos(&m->at);
        if (!PostMessageW(g.hwnd, WM_APP_CTXMENU, 0, (LPARAM)m)) { delete m; return false; }
        return true;
    }
    // Password manager messages from a page (see PW_HOOK_JS). The browser process decides everything from
    // what it knows itself: the address of the frame, never what the page says its address is.
    void OnPw(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefListValue> a) {
        if (g.incognito || !frame || !frame->IsMain()) return;
        Tab* t = TabOf(browser);
        if (!t) return;
        std::wstring origin = OriginOf(frame->GetURL().ToWString());
        if (origin.rfind(L"http://", 0) != 0 && origin.rfind(L"https://", 0) != 0) return;
        std::string kind = a->GetString(0);
        auto reply = [&](const char* k, const std::wstring& x) {
            CefRefPtr<CefProcessMessage> m = CefProcessMessage::Create("nn.pw");
            CefRefPtr<CefListValue> l = m->GetArgumentList();
            l->SetString(0, k); l->SetString(1, x); l->SetString(2, std::wstring());
            return m;
        };
        auto wipe = [](PwPending& p) { if (!p.pass.empty()) SecureZeroMemory(&p.pass[0], p.pass.size() * sizeof(wchar_t)); p = PwPending(); };
        auto ask = [&]() {
            if (t->pw.origin.empty() || GetTickCount64() - t->pw.at > 25000) { wipe(t->pw); return; }
            auto* r = new DlgReq;
            r->kind = DlgReq::PWSAVE; r->tabId = t->id; r->origin = t->pw.origin; r->defText = t->pw.user; r->secret = t->pw.pass;
            wipe(t->pw);
            if (!QueueDialog(r)) {}   // QueueDialog deletes r when it fails
        };
        if (kind == "list") {
            std::vector<std::wstring> users = PwUsers(origin);
            if (users.empty()) return;
            std::wstring j = L"[";
            for (size_t i = 0; i < users.size(); i++) { if (i) j += L","; j += JsonStr(users[i]); }
            frame->SendProcessMessage(PID_RENDERER, reply("list", j + L"]"));
        } else if (kind == "get") {
            std::wstring user = a->GetString(1).ToWString(), pass;
            if (!PwFind(origin, user, pass)) return;
            CefRefPtr<CefProcessMessage> m = reply("fill", user);
            m->GetArgumentList()->SetString(2, pass);
            frame->SendProcessMessage(PID_RENDERER, m);
            SecureZeroMemory(&pass[0], pass.size() * sizeof(wchar_t));
        } else if (kind == "submit") {
            if (!g.savePasswords || PwNeverHas(origin)) return;
            CefRefPtr<CefValue> v = CefParseJSON(a->GetString(1), JSON_PARSER_RFC);
            if (!v || v->GetType() != VTYPE_DICTIONARY) return;
            CefRefPtr<CefDictionaryValue> d = v->GetDictionary();
            std::wstring u = d->GetString("u").ToWString(), p = d->GetString("p").ToWString();
            if (p.empty() || p.size() > 500 || u.size() > 300 || PwSame(origin, u, p)) return;
            wipe(t->pw);
            t->pw.origin = origin; t->pw.user = u; t->pw.pass = p; t->pw.at = GetTickCount64();
        } else if (kind == "ok") {
            ask();   // the login form went away on the same page
        } else if (kind == "page") {
            if (t->pw.origin.empty()) return;
            if (a->GetString(1) == "0") ask(); else wipe(t->pw);   // a new page with no password field: the login worked
        }
    }
    // The renderer reports videos this build cannot play and sites that ask for DRM.
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() == "nn.pid") {
            if (Tab* pt = TabOf(browser)) { pt->pid = (DWORD)message->GetArgumentList()->GetInt(0); ApplyBackgroundLimit(); }
            if (frame && frame->IsMain()) SendSmooth(frame);   // a new page: tell it the smooth scrolling settings
            return true;
        }
        if (message->GetName() == "nn.pw") { OnPw(browser, frame, message->GetArgumentList()); return true; }
        if (message->GetName() != "nn.media") return false;
        std::string kind = message->GetArgumentList()->GetString(0);
        if (kind == "audio") {   // a media element in the page is playing sound; repeated every second while it does
            if (Tab* at = TabOf(browser)) at->audioUntil = GetTickCount64() + 3500;
            return true;
        }
        if (kind == "dirty") {   // a text field holds typed text (or no longer does): see MEDIA_HOOK_JS
            if (Tab* dt = TabOf(browser)) dt->dirty = message->GetArgumentList()->GetString(1) == "1";
            return true;
        }
        Tab* t = TabOf(browser);
        if (!t || t->id != g.active || g.closing) return true;
        if (kind == "openplayer") {
            // the user clicked play on a video that failed: make a temporary copy for the system player
            std::wstring url = message->GetArgumentList()->GetString(1).ToWString();
            static ULONGLONG lastOpen = 0;   // a double click must not start two downloads
            ULONGLONG now = GetTickCount64();
            if ((url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) || !g.tempDl.empty() || now - lastOpen < 5000)
                return true;
            lastOpen = now;
            g.pendingPlayerUrl = url;
            browser->GetHost()->StartDownload(url);
            return true;
        }
        if (kind == "mediadoc") {
            // the whole page is a video that failed to play: offer the system player right away
            std::wstring url = frame->GetURL().ToWString();
            std::wstring name = url.substr(0, url.find_first_of(L"?#"));
            name = PercentDecode(name.substr(name.find_last_of(L'/') + 1));
            auto* ask = new MediaAsk{ nullptr, 0, url, name, message->GetArgumentList()->GetString(1).ToWString(), browser };
            if (!PostMessageW(g.hwnd, WM_APP_MEDIA, 0, (LPARAM)ask)) delete ask;
            return true;
        }
        if (kind == "drm")
            ShowAlert(L"THIS SITE NEEDS DRM (WIDEVINE) - NEONNET CANNOT PLAY PROTECTED VIDEO");
        else if (kind == "media" && g.alert.find(L"DRM") == std::wstring::npos)   // the DRM message already explains it
            ShowAlert(L"VIDEO FORMAT NOT SUPPORTED - CLICK PLAY TO OPEN IT IN YOUR MEDIA PLAYER");
        return true;
    }
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params,
                              int command_id, EventFlags) override {
        if (command_id == MENU_OPEN_LINK_TAB) {
            std::wstring link = params->GetLinkUrl().ToWString();
            if (link.rfind(L"http", 0) == 0) CreateTab(link, false);
            return true;
        }
        if (command_id == MENU_VIEW_SOURCE) {
            std::wstring u = browser->GetMainFrame()->GetURL().ToWString();
            if (u.rfind(L"http", 0) == 0 || u.rfind(L"file:", 0) == 0) CreateTab(L"view-source:" + u, true);
            return true;
        }
        if (command_id == MENU_OPEN_IMAGE_TAB) {
            std::wstring u = params->GetSourceUrl().ToWString();
            if (u.rfind(L"http", 0) == 0) CreateTab(u, false);
            return true;
        }
        if (command_id == MENU_OPEN_LINK_INCOGNITO) {
            std::wstring u = params->GetLinkUrl().ToWString();
            if (u.rfind(L"http", 0) == 0) LaunchIncognito(u);
            return true;
        }
        if (command_id == MENU_SEARCH_SELECTION) {
            std::wstring q = params->GetSelectionText().ToWString();
            if (!q.empty() && q.size() < 2000) CreateTab(std::wstring(CurrentEngine().url) + CefURIEncode(q, false).ToWString(), false);
            return true;
        }
        if (command_id == MENU_OPEN_PLAYER) {
            // Download a temporary copy (marked by its URL, see OnBeforeDownload) and hand it to the system player.
            g.pendingPlayerUrl = params->GetSourceUrl().ToWString();
            browser->GetHost()->StartDownload(params->GetSourceUrl());
            return true;
        }
        if (command_id == MENU_INSPECT) {
            if (Tab* t = TabOf(browser)) OpenDevTools(t, CefPoint(params->GetXCoord(), params->GetYCoord()));
            return true;
        }
        return false;
    }

    IMPLEMENT_REFCOUNTING(Client);
};

static CefRefPtr<Client> g_client;
static CefRefPtr<CefClient> MainClient() { return g_client.get(); }

// ---------------------------------------------------------------------------
// https://neonnet.ui/* : embedded pages + command channel for the chrome UI

class StringResource : public CefResourceHandler {
public:
    StringResource(std::string data, std::string mime, int status) : data_(std::move(data)), mime_(std::move(mime)), status_(status) {}
    bool Open(CefRefPtr<CefRequest>, bool& handle_request, CefRefPtr<CefCallback>) override { handle_request = true; return true; }
    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& length, CefString&) override {
        response->SetStatus(status_);
        response->SetStatusText(status_ == 200 ? "OK" : status_ == 204 ? "No Content" : status_ == 403 ? "Forbidden" :
                                status_ == 400 ? "Bad Request" : "Not Found");
        response->SetMimeType(mime_);
        response->SetCharset("utf-8");
        CefResponse::HeaderMap h;
        h.insert(std::make_pair("Cache-Control", "no-store"));
        h.insert(std::make_pair("X-Frame-Options", "DENY"));   // NeonNet pages never load inside other sites
        h.insert(std::make_pair("X-Content-Type-Options", "nosniff"));
        response->SetHeaderMap(h);
        length = (int64_t)data_.size();
    }
    bool Read(void* out, int want, int& got, CefRefPtr<CefResourceReadCallback>) override {
        got = 0;
        if (pos_ >= data_.size()) return false;
        got = (int)std::min<size_t>(want, data_.size() - pos_);
        memcpy(out, data_.data() + pos_, got);
        pos_ += got;
        return true;
    }
    void Cancel() override {}
private:
    std::string data_, mime_;
    int status_;
    size_t pos_ = 0;
    IMPLEMENT_REFCOUNTING(StringResource);
};

static std::wstring QueryParam(const std::wstring& query, const wchar_t* name) {
    std::wstring key = std::wstring(name) + L"=";
    for (size_t p = 0; p < query.size();) {
        size_t amp = query.find(L'&', p);
        if (amp == std::wstring::npos) amp = query.size();
        if (query.compare(p, key.size(), key) == 0) return PercentDecode(query.substr(p + key.size(), amp - p - key.size()));
        p = amp + 1;
    }
    return {};
}

static bool HasQueryParam(const std::wstring& query, const wchar_t* name) {
    std::wstring key = std::wstring(name) + L"=";
    for (size_t p = 0; p < query.size();) {
        size_t amp = query.find(L'&', p);
        if (amp == std::wstring::npos) amp = query.size();
        if (query.compare(p, key.size(), key) == 0) return true;
        p = amp + 1;
    }
    return false;
}

static std::wstring RegGetString(const wchar_t* name);
static bool AutoUpdateEnabled();
static bool IsInstalledCopy();

static std::string JsonUtf8(const std::wstring& w) {
    std::string s = CefString(w).ToString(), o = "\"";
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '<') o += "\\u003c";
        else if (u < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", u); o += b; }
        else o += c;
    }
    return o + "\"";
}

// {"sites":[{"origin":"https://x.com","perms":[{"k":"camera","l":"Camera","v":1}]}]} for neonnet://permissions
static std::string SpJsonList() {
    std::lock_guard<std::mutex> lk(g_spMu);
    std::string o = "{\"sites\":[";
    bool firstSite = true;
    for (const auto& s : g_sp) {
        if (!firstSite) o += ",";
        firstSite = false;
        o += "{\"origin\":" + JsonUtf8(s.first) + ",\"perms\":[";
        bool first = true;
        for (const auto& d : PERM_DEFS) {
            auto e = s.second.find(d.key);
            if (e == s.second.end()) continue;
            if (!first) o += ",";
            first = false;
            o += "{\"k\":" + JsonUtf8(d.key) + ",\"l\":" + JsonUtf8(d.label) + ",\"v\":" + std::to_string(e->second) + "}";
        }
        o += "]}";
    }
    return o + "]}";
}

static std::string PwJsonList() {
    std::lock_guard<std::mutex> lk(g_pwMu);
    std::string o = "{\"saved\":[";
    for (size_t i = 0; i < g_pw.size(); i++) {
        if (i) o += ",";
        o += "{\"id\":" + std::to_string(g_pw[i].id) + ",\"origin\":" + JsonUtf8(g_pw[i].origin) + ",\"user\":" + JsonUtf8(g_pw[i].user) + "}";
    }
    o += "],\"never\":[";
    bool first = true;
    for (const auto& n : g_pwNever) { if (!first) o += ","; first = false; o += JsonUtf8(n); }
    return o + "]}";
}

// Settings for the settings page. Runs on the IO thread: only atomics and the registry.
static std::string SettingsJson() {
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::string o = "{\"searchUrl\":" + JsonUtf8(CurrentEngine().url) + ",\"search\":" + JsonUtf8(CurrentEngine().id) + ",\"engines\":[";
    for (int i = 0; i < SEARCH_ENGINE_COUNT; i++) {
        if (i) o += ",";
        o += "{\"id\":" + JsonUtf8(SEARCH_ENGINES[i].id) + ",\"name\":" + JsonUtf8(SEARCH_ENGINES[i].name) + "}";
    }
    std::wstring chromium = std::to_wstring(cef_version_info(4)) + L"." + std::to_wstring(cef_version_info(5)) + L"." +
                            std::to_wstring(cef_version_info(6)) + L"." + std::to_wstring(cef_version_info(7));
    size_t scriptTotal = 0, scriptEnabled = 0;
    userscripts::Counts(scriptTotal, scriptEnabled);
    UpdSnap us;
    { std::lock_guard<std::mutex> lk(g_updMu); us = g_updSnap; }
    const char* updState = "idle";
    if (us.state == U_CHECKING) updState = "checking";
    else if (us.state == U_DOWNLOADING) updState = "downloading";
    else if (us.state == U_AVAILABLE) updState = "available";
    else if (us.state == U_READY) updState = "ready";
    o += std::string("],\"incognito\":") + b(g.incognito) + ",\"scan\":" + b(g.scan) + ",\"autoUpdate\":" + b(AutoUpdateEnabled()) + ",\"installed\":" + b(IsInstalledCopy()) +
         ",\"updState\":\"" + updState + "\",\"updPercent\":" + std::to_string(us.percent) + ",\"updVersion\":" + JsonUtf8(us.version) + ",\"updResult\":" + JsonUtf8(us.result) + ",\"updError\":" + JsonUtf8(us.error) +
         ",\"home\":" + JsonUtf8(RegGetString(L"HomePage")) + ",\"askDownload\":" + b(RegGetDword(L"AskDownload", 1) != 0) +
         ",\"httpsOnly\":" + b(RegGetDword(L"HttpsOnly", 0) != 0) + ",\"smoothScroll\":" + b(RegGetDword(L"SmoothScroll", 1) != 0) + ",\"smoothStep\":" + std::to_string(RegGetDword(L"SmoothStep", 120)) + ",\"smoothTime\":" + std::to_string(RegGetDword(L"SmoothTime", 400)) + ",\"netSpeed\":" + b(RegGetDword(L"NetSpeed", 1) != 0) + ",\"devDock\":" + std::to_string(RegGetDword(L"DevToolsDock", 0)) + ",\"defaultBrowser\":" + b(browserreg::IsDefault()) + ",\"blockAds\":" + b(g.blockAds.load()) + ",\"savePasswords\":" + b(RegGetDword(L"SavePasswords", 1) != 0) + ",\"passwordCount\":" + std::to_string(PwCount()) + ",\"blocked\":" + std::to_string(g.blockedCount.load()) + ",\"favicons\":" + b(RegGetDword(L"ShowFavicons", 1) != 0) + ",\"restoreSession\":" + b(RegGetDword(L"RestoreSession", 0) != 0) +
         ",\"scriptTotal\":" + std::to_string(scriptTotal) + ",\"scriptEnabled\":" + std::to_string(scriptEnabled) +
         ",\"downloadDir\":" + JsonUtf8(KnownFolder(FOLDERID_Downloads)) + ",\"bookmarksBar\":" + b(RegGetDword(L"BookmarksBar", 1) != 0) + ",\"version\":" + JsonUtf8(APP_VERSION) +
         ",\"chromium\":" + JsonUtf8(chromium) + "}";
    return o;
}

// https://neonnet.ui/api/* : history, bookmarks, downloads and settings for NeonNet's own
// pages. Only a main frame that shows a neonnet.ui page may call it, so websites can neither
// read nor change anything.
static CefRefPtr<CefResourceHandler> ServeApi(const std::wstring& path, const std::wstring& query, const std::string& body) {
    std::string out = "{}";
    bool changed = false;
    long long id = _wtoi64(QueryParam(query, L"id").c_str());
    if (path == L"api/history") out = store::HistoryJson(QueryParam(query, L"q"), 1500);
    else if (path == L"api/history/delete") changed = store::DeleteHistory(id);
    else if (path == L"api/bookmarks") out = store::BookmarksJson();
    else if (path == L"api/bookmarks/add") {
        if (g.incognito) return new StringResource("{\"error\":\"incognito\"}", "application/json", 403);
        long long nid = store::AddBookmark(QueryParam(query, L"url"), QueryParam(query, L"title"), QueryParam(query, L"parent"));
        if (!nid) return new StringResource("{\"error\":\"invalid address\"}", "application/json", 400);
        changed = true;
        out = "{\"id\":" + std::to_string(nid) + "}";
    }
    else if (path == L"api/bookmarks/folder") {
        if (g.incognito) return new StringResource("{\"error\":\"incognito\"}", "application/json", 403);
        long long nid = store::AddFolder(QueryParam(query, L"title"), QueryParam(query, L"parent"));
        if (!nid) return new StringResource("{\"error\":\"invalid or duplicate folder\"}", "application/json", 400);
        changed = true;
        out = "{\"id\":" + std::to_string(nid) + "}";
    }
    else if (path == L"api/bookmarks/delete") changed = store::DeleteBookmark(id);
    else if (path == L"api/bookmarks/edit") {
        if (!store::EditBookmark(id, QueryParam(query, L"title"), QueryParam(query, L"url"),
                                 QueryParam(query, L"parent"), HasQueryParam(query, L"parent")))
            return new StringResource("{\"error\":\"could not save\"}", "application/json", 400);
        changed = true;
    }
    else if (path == L"api/bookmarks/export") {
        return new StringResource(store::ExportBookmarksHtml(), "text/html; charset=utf-8", 200);
    }
    else if (path == L"api/bookmarks/import") {
        if (g.incognito) return new StringResource("{\"error\":\"incognito\"}", "application/json", 403);
        int added = 0, folders = 0;
        if (!store::ImportBookmarksHtml(body, added, folders))
            return new StringResource("{\"error\":\"could not import\"}", "application/json", 400);
        if (added > 0 || folders > 0) changed = true;
        out = "{\"added\":" + std::to_string(added) + ",\"folders\":" + std::to_string(folders) + "}";
    }
    else if (path == L"api/cleardata/run") {
        // The time range is a name, turned into a cut-off here; the page never sends a time.
        std::wstring range = QueryParam(query, L"range");
        long long span = -1;
        if (range == L"hour") span = 60LL * 60 * 1000;
        else if (range == L"day") span = 24LL * 60 * 60 * 1000;
        else if (range == L"week") span = 7LL * 24 * 60 * 60 * 1000;
        else if (range == L"month") span = 28LL * 24 * 60 * 60 * 1000;
        else if (range == L"all") span = 0;
        bool cookies = QueryParam(query, L"cookies") == L"1", cache = QueryParam(query, L"cache") == L"1";
        bool history = QueryParam(query, L"history") == L"1";
        if (span < 0 || (!cookies && !cache && !history))
            return new StringResource("{\"error\":\"invalid request\"}", "application/json", 400);
        if (g.clearState.load() == 1) return new StringResource("{\"error\":\"already running\"}", "application/json", 409);
        auto* req = new ClearReq;
        req->cookies = cookies; req->cache = cache; req->history = history;
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        long long nowMs = (((long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000) - 11644473600000LL;
        req->cutoffMs = span ? nowMs - span : 0;
        g.clearState = 1; g.clearCookies = 0; g.clearSites = 0; g.clearHistory = 0;
        if (!PostMessageW(g.hwnd, WM_APP_CLEARDATA, 0, (LPARAM)req)) { delete req; g.clearState = 0; }
        out = "{\"started\":true}";
    }
    else if (path == L"api/cleardata/status") {
        out = "{\"state\":" + std::to_string(g.clearState.load()) + ",\"cookies\":" + std::to_string(g.clearCookies.load()) +
              ",\"sites\":" + std::to_string(g.clearSites.load()) + ",\"history\":" + std::to_string(g.clearHistory.load()) + "}";
    }
    else if (path == L"api/downloads") out = store::DownloadsJson();
    else if (path == L"api/downloads/remove") changed = store::DownloadRemove(id);
    else if (path == L"api/downloads/clear") { store::DownloadsClear(); changed = true; }
    else if (path == L"api/downloads/cancel" || path == L"api/downloads/open" || path == L"api/downloads/show") {
        // The page sends only an id; what to open is looked up here, never taken from the request.
        WPARAM act = path == L"api/downloads/cancel" ? DLA_CANCEL : path == L"api/downloads/open" ? DLA_OPEN : DLA_SHOW;
        PostMessageW(g.hwnd, WM_APP_DOWNLOAD, act, (LPARAM)id);
    }
    else if (path == L"api/passwords") out = PwJsonList();
    else if (path == L"api/passwords/delete") PwDelete((int)id);
    else if (path == L"api/passwords/never/remove") PwNeverRemove(QueryParam(query, L"origin"));
    else if (path == L"api/passwords/clear") PwClear();
    else if (path == L"api/siteperms") out = SpJsonList();
    else if (path == L"api/siteperms/set" || path == L"api/siteperms/reset" || path == L"api/siteperms/clear") {
        std::wstring origin = QueryParam(query, L"origin"), key = QueryParam(query, L"key"), val = QueryParam(query, L"value");
        if (path == L"api/siteperms/clear") SpClear();
        else if (!SpOriginValid(origin)) return new StringResource("{\"error\":\"invalid site\"}", "application/json", 400);
        else if (path == L"api/siteperms/reset") SpReset(origin);
        else if (PermKeyValid(key) && (val == L"0" || val == L"1" || val == L"2")) SpSet(origin, key, _wtoi(val.c_str()));
        else return new StringResource("{\"error\":\"invalid value\"}", "application/json", 400);
        PostMessageW(g.hwnd, WM_APP_STORE, 0, 0);   // the lock icon panel shows the new values
    }
    else if (path == L"api/userscripts") out = userscripts::ListJson();
    else if (path == L"api/userscripts/save") {
        std::string err;
        long long savedId = userscripts::SaveFromJson(body, err);
        if (!savedId) {
            int code = userscripts::IsReadOnly() ? 403 : 400;
            std::wstring werr(err.begin(), err.end());
            return new StringResource("{\"error\":" + JsonUtf8(werr) + "}", "application/json", code);
        }
        out = "{\"ok\":true,\"id\":" + std::to_string(savedId) + "}";
    }
    else if (path == L"api/userscripts/toggle") {
        if (!userscripts::SetEnabled(id, QueryParam(query, L"enabled") == L"1"))
            return new StringResource("{\"error\":\"cannot update script\"}", "application/json", userscripts::IsReadOnly() ? 403 : 400);
    }
    else if (path == L"api/userscripts/delete") {
        if (!userscripts::Delete(id))
            return new StringResource("{\"error\":\"cannot delete script\"}", "application/json", userscripts::IsReadOnly() ? 403 : 400);
    }
    else if (path == L"api/settings") out = SettingsJson();
    else if (path == L"api/settings/set") {
        std::wstring key = QueryParam(query, L"key"), val = QueryParam(query, L"value");
        bool ok = false;
        if (key == L"search") { for (const auto& e : SEARCH_ENGINES) if (val == e.id) ok = true; }
        else if (key == L"scan" || key == L"autoUpdate" || key == L"askDownload" || key == L"restoreSession" || key == L"favicons" || key == L"bookmarksBar" || key == L"httpsOnly" || key == L"blockAds" || key == L"savePasswords" || key == L"smoothScroll" || key == L"netSpeed") ok = (val == L"0" || val == L"1");
        else if (key == L"smoothStep") ok = (val == L"70" || val == L"120" || val == L"200");
        else if (key == L"devDock") ok = (val == L"0" || val == L"1" || val == L"2" || val == L"3");
        else if (key == L"defaultBrowser") ok = (val == L"1");
        else if (key == L"checkUpdate") ok = (val == L"1" || val == L"restart" || val == L"download");
        else if (key == L"smoothTime") ok = (val == L"250" || val == L"400" || val == L"700");
        else if (key == L"proceed") ok = val.size() < 2100 && (val.rfind(L"cert|https://", 0) == 0 || val.rfind(L"http|http://", 0) == 0);
        else if (key == L"home") {
            std::wstring low = val; for (auto& ch : low) ch = towlower(ch);
            ok = val.empty() || (val.size() < 500 && (low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0 || low.rfind(L"file:///", 0) == 0));
        }
        if (!ok) return new StringResource("{\"error\":\"invalid value\"}", "application/json", 400);
        // Applied on the UI thread, and waited for, so that the page can read the result back.
        std::pair<std::wstring, std::wstring> msg(key, val);
        DWORD_PTR res = 0;
        SendMessageTimeoutW(g.hwnd, WM_APP_SETTING, 0, (LPARAM)&msg, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &res);
        out = "{\"ok\":true}";
    }
    else return new StringResource("not found", "text/plain", 404);
    if (changed) PostMessageW(g.hwnd, WM_APP_STORE, 0, 0);
    return new StringResource(std::move(out), "application/json", 200);
}

class UiSchemeFactory : public CefSchemeHandlerFactory {
public:
    // Runs on the IO thread.
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString&,
                                         CefRefPtr<CefRequest> request) override {
        std::wstring url = request->GetURL().ToWString();
        std::wstring path = url.substr(url.rfind(CHROME_UI_HOST, 0) == 0 ? wcslen(CHROME_UI_HOST) : wcslen(UI_HOST));
        std::wstring query;
        size_t q = path.find(L'?');
        if (q != std::wstring::npos) { query = path.substr(q + 1); path.resize(q); }
        size_t hash = path.find(L'#');
        if (hash != std::wstring::npos) path.resize(hash);

        if (path == L"cmd") {
            // Only the chrome UI browser may send commands.
            if (browser && (browser->GetIdentifier() == g.chromeId.load() ||
                            (g.panelId.load() && browser->GetIdentifier() == g.panelId.load())) && query.rfind(L"m=", 0) == 0) {
                auto* msg = new std::wstring(PercentDecode(query.substr(2)));
                if (!PostMessageW(g.hwnd, WM_APP_CMD, 0, (LPARAM)msg)) delete msg;
            }
            return new StringResource("", "text/plain", 204);
        }
        if (path == L"suggest") {
            // Address bar suggestions: only for the toolbar.
            if (!(browser && browser->GetIdentifier() == g.chromeId.load())) return new StringResource("", "text/plain", 403);
            return new StringResource(store::SuggestJson(QueryParam(query, L"q"), 8), "application/json", 200);
        }
        if (path == L"bookmarks") {
            // Bookmarks bar folder contents: only for the toolbar.
            if (!(browser && browser->GetIdentifier() == g.chromeId.load())) return new StringResource("", "text/plain", 403);
            return new StringResource(store::BookmarksBarJson(QueryParam(query, L"parent")), "application/json", 200);
        }
        if (path.rfind(L"api/", 0) == 0) {
            bool trusted = browser && browser->GetIdentifier() != g.chromeId.load() && frame && frame->IsMain() &&
                           frame->GetURL().ToWString().rfind(UI_HOST, 0) == 0;
            if (!trusted) return new StringResource("", "text/plain", 403);
            std::string body;
            if (CefRefPtr<CefPostData> pd = request->GetPostData()) {
                CefPostData::ElementVector els;
                pd->GetElements(els);
                for (auto& el : els) {
                    if (el->GetType() == PDE_TYPE_BYTES && el->GetBytesCount() > 0 && body.size() + el->GetBytesCount() <= 16 * 1024 * 1024) {
                        size_t off = body.size();
                        body.resize(off + el->GetBytesCount());
                        el->GetBytes(el->GetBytesCount(), body.data() + off);
                    }
                }
            }
            return ServeApi(path, query, body);
        }
        int id = 0;
        if (path == L"chrome.html") id = IDR_CHROME;
        else if (path == L"newtab.html" || path.empty()) id = IDR_NEWTAB;
        else if (path == L"history.html") id = IDR_HISTORY;
        else if (path == L"bookmarks.html") id = IDR_BOOKMARKS;
        else if (path == L"downloads.html") id = IDR_DOWNLOADS;
        else if (path == L"settings.html") id = IDR_SETTINGS;
        else if (path == L"performance.html") id = IDR_PERFORMANCE;
        else if (path == L"warning.html") id = IDR_WARNING;
        else if (path == L"passwords.html") id = IDR_PASSWORDS;
        else if (path == L"permissions.html") id = IDR_PERMISSIONS;
        else if (path == L"extensions.html" || path == L"userscripts.html") id = IDR_EXTENSIONS;
        std::string body;
        if (id && LoadUi(id, body)) return new StringResource(std::move(body), "text/html", 200);
        // Unknown neonnet:// page: a themed page with a proper title (the tab would otherwise
        // be named after the address). Nothing from the request is echoed back.
        static const char NOT_FOUND[] =
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>NOT FOUND</title><style>"
            "html,body{margin:0;height:100%;background:#12062c;color:#e8d8ff;font:14px Consolas,'Courier New',monospace}"
            "main{max-width:640px;margin:0 auto;padding:12vh 20px}"
            "h1{margin:0;font:italic 900 56px Impact,'Arial Black',sans-serif;letter-spacing:3px;color:#00e5ff;text-shadow:3px 3px 0 #ff2e97}"
            "p{color:#8a6fc0;letter-spacing:1px}a{color:#ffd600;text-decoration:none}a:hover{text-decoration:underline}"
            "</style></head><body><main><h1>404</h1><p>&gt; NO SUCH PAGE ON THIS NODE</p>"
            "<p><a href=\"https://neonnet.ui/newtab.html\">&gt; BACK TO THE GRID</a></p></main></body></html>";
        return new StringResource(NOT_FOUND, "text/html", 404);
    }
    IMPLEMENT_REFCOUNTING(UiSchemeFactory);
};

// ---------------------------------------------------------------------------
// Auto update
//
// The installed copy checks the latest GitHub release of UPDATE_REPO at most once a day.
// A newer NeonNet-Setup.exe is downloaded in the background, verified against the
// release's NeonNet-Setup.exe.sha256, and run with --silent after NeonNet closes.
// A copy running from anywhere else (for example build\app) only reports new versions.

static std::wstring UpdatesDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\Updates"; }

// The folder the installer registered for this user ("InstallLocation" of the Uninstall entry in HKCU,
// or HKLM for an install for all users), empty when there is none.
static std::wstring RegisteredInstallDir(HKEY root) {
    wchar_t buf[MAX_PATH] = {}; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, UNINSTALL_KEY, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}
static std::wstring SelfDir() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    return self;
}
static bool SameDir(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    std::wstring x = a, y = b;
    while (x.size() > 3 && x.back() == L'\\') x.pop_back();
    while (y.size() > 3 && y.back() == L'\\') y.pop_back();
    return _wcsicmp(x.c_str(), y.c_str()) == 0;
}
// This copy was installed for this user only (in the folder the installer was told to use). Only such a copy
// updates itself: an install for all users lives where the user cannot write, so it only reports new versions.
static bool IsInstalledCopy() {
    std::wstring self = SelfDir();
    return SameDir(self, RegisteredInstallDir(HKEY_CURRENT_USER)) ||
           SameDir(self, KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\" + APP_DIR_NAME);
}
static bool IsMachineInstall() { return SameDir(SelfDir(), RegisteredInstallDir(HKEY_LOCAL_MACHINE)); }

static bool AutoUpdateEnabled() { return RegGetDword(L"AutoUpdate", 1) != 0; }

static ULONGLONG NowMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000;
}

static ULONGLONG RegGetQword(const wchar_t* name) {
    ULONGLONG v = 0; DWORD sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_QWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return 0;
    return v;
}

static std::wstring RegGetString(const wchar_t* name) {
    wchar_t buf[512] = {}; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    return buf;
}

// Home button and start-up page: the page chosen in the settings, else the new tab page.
static std::wstring HomeUrl() {
    std::wstring h = RegGetString(L"HomePage");
    return h.empty() ? std::wstring(NEWTAB) : h;
}

// ---------------------------------------------------------------------------
// Tabs of the last session
//
// NeonNet writes the open tabs to %LOCALAPPDATA%\NeonNet\session.txt itself (Chromium's own
// session restore needs persistent session cookies, which stay off, see ARCHITECTURE.md). One
// line per tab: flags (P pinned, A active, - none), title, address, separated by tabs. Pinned
// tabs always come back; the others only when "continue where you left off" is on. Restored
// tabs start asleep (no page loaded), so thirty tabs do not mean thirty pages loading at once.
// An incognito window neither writes nor reads the file.

struct SavedTab { std::wstring url, title; bool pinned = false, active = false; int wsId = 1; };

static bool SessionUrlOk(const std::wstring& u) {
    if (u.empty() || u.size() > 8000) return false;
    for (wchar_t c : u) if (c < 0x20) return false;
    return u.rfind(L"http://", 0) == 0 || u.rfind(L"https://", 0) == 0 || u.rfind(L"file:///", 0) == 0;
}

static std::wstring SessionPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\session.txt"; }

static std::string SessionText() {
    std::string o = "NEONNET-SESSION 1\n";
    int n = 0;
    for (auto& t : g.tabs) {
        std::wstring u = DisplayUrl(t.get());
        if (!SessionUrlOk(u) || n >= 200) continue;
        n++;
        std::wstring ti = t->title;
        for (auto& c : ti) if (c < 0x20) c = L' ';
        if (ti.size() > 200) ti.resize(200);
        std::string flags;
        if (t->pinned) flags += 'P';
        if (t->id == g.active) flags += 'A';
        if (t->wsId > 1) flags += "W" + std::to_string(t->wsId);
        if (flags.empty()) flags = "-";
        o += flags + "\t" + Utf8(ti) + "\t" + Utf8(u) + "\n";
    }
    return o;
}

// Writes session.txt now (a new file, then swapped in, so a crash cannot leave half a file).
static void SaveSession() {
    g.sessionPending = false;
    if (g.hwnd) KillTimer(g.hwnd, TIMER_SESSION);
    if (g.incognito || g.tabs.empty()) return;
    std::string s = SessionText();
    if (s == g.lastSession) return;
    std::wstring path = SessionPath(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) g.lastSession = s;
    else DeleteFileW(tmp.c_str());
}

static std::vector<SavedTab> LoadSession() {
    std::vector<SavedTab> v;
    HANDLE h = CreateFileW(SessionPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string data;
    if (size > 0 && size < 4 * 1024 * 1024) {
        data.resize(size);
        if (!ReadFile(h, &data[0], size, &got, nullptr) || got != size) data.clear();
    }
    CloseHandle(h);
    size_t pos = data.find('\n');
    if (pos == std::string::npos || data.compare(0, pos, "NEONNET-SESSION 1") != 0) return v;
    g.lastSession = data;
    for (++pos; pos < data.size() && v.size() < 200;) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(pos, e - pos);
        pos = e + 1;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        SavedTab s;
        std::string fl = line.substr(0, t1);
        s.pinned = fl.find('P') != std::string::npos;
        s.active = fl.find('A') != std::string::npos;
        size_t wp = fl.find('W');
        if (wp != std::string::npos) {
            int wv = atoi(fl.substr(wp + 1).c_str());
            if (wv > 0) s.wsId = wv;
        }
        s.title = Wide(line.substr(t1 + 1, t2 - t1 - 1));
        s.url = Wide(line.substr(t2 + 1));
        if (SessionUrlOk(s.url)) v.push_back(std::move(s));
    }
    return v;
}

// The first tabs of a new window: the saved ones that come back, then (or instead) the
// address NeonNet was started with, or the home page.
static void OpenStartTabs() {
    LoadWorkspaces();
    std::vector<SavedTab> saved;
    bool all = false;
    if (!g.incognito) {
        saved = LoadSession();
        all = RegGetDword(L"RestoreSession", 0) != 0;
        // A lone empty new tab is not worth bringing back: the home page is better.
        if (all && saved.size() == 1 && !saved[0].pinned && saved[0].url.rfind(NEWTAB, 0) == 0) saved.clear();
        if (!all) saved.erase(std::remove_if(saved.begin(), saved.end(), [](const SavedTab& s) { return !s.pinned; }), saved.end());
    }
    int activeId = 0;
    for (auto& s : saved) {
        auto tab = std::make_unique<Tab>();
        tab->id = g.nextId++;
        tab->url = s.url;
        if (!s.title.empty()) tab->title = s.title;
        tab->pinned = s.pinned;
        tab->wsId = FindWs(s.wsId) ? s.wsId : g.activeWs;
        tab->discarded = true;   // asleep until selected, see RestoreTab
        tab->loading = false;
        tab->lastActive = GetTickCount64();
        if (s.active) activeId = tab->id;
        g.tabs.push_back(std::move(tab));
    }
    KeepPinsFirst();
    bool hasActiveWsTab = false;
    for (auto& t : g.tabs) if (t->wsId == g.activeWs) { hasActiveWsTab = true; break; }
    bool openNew = saved.empty() || !all || !g.startUrl.empty() || !hasActiveWsTab;
    if (openNew) CreateTab(g.startUrl.empty() ? HomeUrl() : g.startUrl, true);
    else {
        int pick = activeId;
        if (Tab* act = FindTab(activeId)) {
            if (act->wsId != g.activeWs) pick = 0;
        }
        if (!pick) for (auto& t : g.tabs) if (t->wsId == g.activeWs) { pick = t->id; break; }
        SelectTab(pick, true);
    }
}

static void PostUpdate(const UpdateMsg& m) {
    auto* p = new UpdateMsg(m);
    if (!PostMessageW(g.hwnd, WM_APP_UPDATE, 0, (LPARAM)p)) delete p;
}

static bool HasPayloadFooter(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; GetFileSizeEx(f, &size);
    char magic[8] = {}; DWORD rd = 0;
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - 8;
    bool ok = size.QuadPart > 16 && SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, magic, 8, &rd, nullptr) && rd == 8;
    CloseHandle(f);
    return ok && memcmp(magic, "NEONPAK1", 8) == 0;
}

// Runs on the updater thread: talks to the network and the disk, reports back with PostUpdate().
static void UpdateWorker(bool manual, bool download) {
    std::atomic<bool>* cancel = &g.updCancel;
    UpdateMsg m; m.manual = manual;
    auto fail = [&](const std::wstring& why) { m.state = U_FAILED; m.error = why; PostUpdate(m); };

    std::string json; DWORD status = 0;
    if (!upd::HttpGet(L"https://api.github.com/repos/" UPDATE_REPO L"/releases/latest", &json, nullptr,
                      2 * 1024 * 1024, nullptr, cancel, &status)) {
        return fail(status == 404 ? L"no release published yet" : status ? L"server answered " + std::to_wstring(status) : L"no connection");
    }
    ULONGLONG now = NowMs();
    RegSetBin(L"LastUpdateCheck", REG_QWORD, &now, sizeof(now));

    upd::Release rel; std::wstring err;
    if (!upd::ParseLatest(json, UPDATE_REPO, rel, err)) return fail(err);
    m.version = rel.version; m.page = rel.pageUrl;
    if (upd::CompareVersions(rel.version, APP_VERSION) <= 0) { m.state = U_UPTODATE; PostUpdate(m); return; }
    if (!download) { m.state = U_AVAILABLE; PostUpdate(m); return; }

    std::string shaText;
    if (!upd::HttpGet(rel.shaUrl, &shaText, nullptr, 4096, nullptr, cancel, &status)) return fail(L"checksum download failed");
    std::string want = upd::ParseShaFile(shaText);
    if (want.empty()) return fail(L"checksum file is invalid");

    std::wstring dir = UpdatesDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring dest = dir + L"\\NeonNet-Setup-" + rel.version + L".exe";
    std::string have;
    if (!(upd::Sha256File(dest, have) && have == want)) {
        std::wstring part = dest + L".part";
        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return fail(L"cannot write the update");
        int last = -1;
        std::wstring ver = rel.version;
        bool ok = upd::HttpGet(rel.installerUrl, nullptr, f, 0, [&](ULONGLONG got, ULONGLONG total) {
            int pct = total ? (int)(got * 100 / total) : 0;
            if (pct != last) {
                last = pct;
                UpdateMsg p; p.state = U_DOWNLOADING; p.percent = pct; p.version = ver; p.manual = manual;
                PostUpdate(p);
            }
        }, cancel, &status);
        CloseHandle(f);
        if (!ok || !upd::Sha256File(part, have) || have != want || !HasPayloadFooter(part)) {
            DeleteFileW(part.c_str());
            return fail(ok ? L"download did not match its checksum" : L"download failed");
        }
        DeleteFileW(dest.c_str());
        if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) return fail(L"cannot store the update");
    }
    m.state = U_READY; m.path = dest;
    PostUpdate(m);
}

static DWORD WINAPI UpdateThread(LPVOID arg) {
    int flags = (int)(INT_PTR)arg;
    UpdateWorker((flags & 1) != 0, (flags & 2) != 0);
    return 0;
}

static void ShowNotice(const std::wstring& text) {
    g.notice = text;
    SetTimer(g.hwnd, TIMER_NOTICE, 8000, nullptr);
    InvalidateStatus();
}

static void StartUpdateCheck(bool manual) {
    if (g.closing || g.incognito || g.upd == U_CHECKING || g.upd == U_DOWNLOADING) return;
    if (g.upd == U_READY) { if (manual) ShowNotice(L"UPDATE v" + g.updVersion + L" IS READY - USE THE MENU TO RESTART"); return; }
    if (g.updThread) { WaitForSingleObject(g.updThread, 0); CloseHandle(g.updThread); g.updThread = nullptr; }
    g.upd = U_CHECKING;
    SyncUpdSnap(L"");
    if (manual) { g.notice = L"CHECKING FOR UPDATES..."; KillTimer(g.hwnd, TIMER_NOTICE); InvalidateStatus(); }
    int flags = (manual ? 1 : 0) | (IsInstalledCopy() ? 2 : 0);
    g.updThread = CreateThread(nullptr, 0, UpdateThread, (LPVOID)(INT_PTR)flags, 0, nullptr);
    if (!g.updThread) { g.upd = U_IDLE; SyncUpdSnap(L"failed", L"Could not start update check."); }
}

static std::wstring Upper(std::wstring s) { for (auto& c : s) c = towupper(c); return s; }

static void OnUpdateMsg(const UpdateMsg& m) {
    switch (m.state) {
    case U_DOWNLOADING: g.upd = U_DOWNLOADING; g.updPercent = m.percent; g.updVersion = m.version; SyncUpdSnap(); break;
    case U_READY: g.upd = U_READY; g.updVersion = m.version; g.updPath = m.path; g.notice.clear(); SyncUpdSnap(L""); break;
    case U_AVAILABLE: g.upd = U_AVAILABLE; g.updVersion = m.version; g.updPage = m.page; g.notice.clear(); SyncUpdSnap(L""); break;
    case U_UPTODATE:
        g.upd = U_IDLE;
        SyncUpdSnap(L"uptodate");
        if (m.manual) ShowNotice(L"NEONNET " APP_VERSION L" IS UP TO DATE");
        break;
    case U_FAILED:
        g.upd = U_IDLE;
        SyncUpdSnap(L"failed", m.error);
        if (m.manual) ShowNotice(L"UPDATE CHECK FAILED: " + Upper(m.error));
        break;
    default: break;
    }
    InvalidateStatus();
}

// Hourly timer: starts a background check when automatic updates are on and the last
// successful check is more than a day old.
static void MaybeAutoCheck() {
    SetTimer(g.hwnd, TIMER_UPDATE, 60 * 60 * 1000, nullptr);
    if (!IsInstalledCopy() || !AutoUpdateEnabled()) return;
    if (NowMs() - RegGetQword(L"LastUpdateCheck") < UPDATE_INTERVAL_MS) return;
    StartUpdateCheck(false);
}

// At startup: remove installers left from earlier updates and report the last update.
static void UpdateStartup() {
    if (g.incognito) return;   // the normal window cleans up and reports updates
    WIN32_FIND_DATAW fd;
    std::wstring dir = UpdatesDir();
    HANDLE f = FindFirstFileW((dir + L"\\NeonNet-Setup-*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::wstring failed = RegGetString(L"UpdateResult");
    std::wstring last = RegGetString(L"LastRunVersion");
    if (!failed.empty()) {
        ShowNotice(L"UPDATE FAILED: " + Upper(failed));
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"UpdateResult"); RegCloseKey(k); }
    } else if (!last.empty() && upd::CompareVersions(APP_VERSION, last) > 0) {
        ShowNotice(L"UPDATED TO NEONNET " APP_VERSION);
    }
    std::wstring cur = APP_VERSION;
    RegSetBin(L"LastRunVersion", REG_SZ, cur.c_str(), (DWORD)((cur.size() + 1) * sizeof(wchar_t)));
    SetTimer(g.hwnd, TIMER_UPDATE, 10 * 1000, nullptr);   // first check shortly after start
}

// After CEF has shut down: run a downloaded installer, which waits for this process to exit.
static void UpdateShutdown() {
    g.updCancel = true;
    if (g.updThread) { WaitForSingleObject(g.updThread, 5000); CloseHandle(g.updThread); g.updThread = nullptr; }
    if (g.upd != U_READY || g.updPath.empty()) return;
    std::wstring cmd = L"\"" + g.updPath + L"\" --silent --wait-pid=" + std::to_wstring(GetCurrentProcessId());
    if (g.updRelaunch) cmd += L" --relaunch";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(g.updPath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------
// Commands

static void ShowAbout() {
    std::wstring v = L"NEON//NET  v" APP_VERSION L"\n\n"
        L"Chromium " + std::to_wstring(cef_version_info(4)) + L"." + std::to_wstring(cef_version_info(5)) + L"." +
        std::to_wstring(cef_version_info(6)) + L"." + std::to_wstring(cef_version_info(7)) +
        L" (bundled, via the Chromium Embedded Framework).\nThird-party licenses: CREDITS.html in the install folder.\n\n"
        L"Shortcuts\n"
        L"  Ctrl+T  new tab          Ctrl+W  close tab\n"
        L"  Ctrl+Shift+T  reopen closed tab\n"
        L"  Ctrl+F  find in page     F3  find next\n"
        L"  Ctrl+D  bookmark page    Ctrl+Shift+O  bookmarks\n"
        L"  Ctrl+H  history          Ctrl+Shift+E  extensions\n"
        L"  Ctrl+L  address bar      Ctrl+Tab  next tab\n"
        L"  F5      reload           Alt+Left/Right  back/forward\n"
        L"  Ctrl +/-/0  zoom         F11  fullscreen\n"
        L"  Ctrl+P  print            F12  developer tools\n"
        L"  F1      user guide";
    MessageBoxW(g.hwnd, v.c_str(), L"About NeonNet", MB_OK | MB_ICONINFORMATION);
}

static void ToggleBookmark() {
    Tab* a = Active();
    if (!a) return;
    std::wstring u = DisplayUrl(a);
    if (!store::Recordable(u)) { ShowNotice(L"THIS PAGE CANNOT BE BOOKMARKED"); return; }
    if (long long id = store::FindBookmark(u)) { store::DeleteBookmark(id); ShowNotice(L"BOOKMARK REMOVED"); }
    else { store::AddBookmark(u, a->title); ShowNotice(L"BOOKMARKED: " + Upper(a->title.empty() ? u : a->title)); }
    g.bmBarDirty = true;
    ScheduleSave();
    SendState();
}

// Shows one of NeonNet's own pages: reuses a tab that already has it, or the active
// tab when it is an empty new tab, otherwise opens a new tab.
static void OpenPage(const std::wstring& page) {
    for (auto& t : g.tabs) {
        if (t->url.rfind(page, 0) == 0 && t->browser) { SelectTab(t->id, true); t->browser->Reload(); return; }
    }
    Tab* a = Active();
    if (a && a->browser && a->url.rfind(NEWTAB, 0) == 0) { a->browser->GetMainFrame()->LoadURL(page); FocusPage(); return; }
    CreateTab(page, true);
}

// An incognito window is a second NeonNet process with its own in-memory profile (like Chrome's
// separate incognito window, but isolated by process). Starting it again while it is open
// does not start a third one: CEF hands the request to the running one, which opens a new tab.
static void LaunchIncognito(const std::wstring& url) {
    wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring args = L"--neonnet-incognito";
    if (url.rfind(L"http", 0) == 0 && url.find(L'"') == std::wstring::npos) args += L" \"" + url + L"\"";
    ShellExecuteW(nullptr, L"open", exe, args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Performance panel: memory and CPU per tab, cleaning, and two optional limiters.
//
// Every renderer reports its process id (message nn.pid, see NeonApp), so a tab's memory and
// CPU are those of its renderer process. Tabs that share a process show the same figures.
// Cleaning asks each page to collect garbage and then trims the working sets of all processes.
// A tab can be unloaded (its browser is closed, the tab stays and reloads when selected). The RAM
// limiter unloads the longest-unused background tab while all processes use more than the limit,
// and the background limiter runs the processes of background tabs at low priority.

static void UpdatePerfTimer() {
    if (!g.hwnd) return;
    if (g.panelOpen || g.ramLimitMB || g.bgLimit) SetTimer(g.hwnd, TIMER_PERF, g.panelOpen ? 2000 : 4000, nullptr);
    else KillTimer(g.hwnd, TIMER_PERF);
}

static void SamplePerf() {
    g.perfSnap = perf::Sample();
    g.perfTotal = g.perfSnap.totalBytes;
    g.perfCpu = g.perfSnap.totalCpu;
    auto self = g.perfSnap.byPid.find(GetCurrentProcessId());
    g.perfBrowser = self != g.perfSnap.byPid.end() ? self->second.bytes : 0;
    for (auto& t : g.tabs) {
        auto p = t->pid ? g.perfSnap.byPid.find(t->pid) : g.perfSnap.byPid.end();
        t->memBytes = p != g.perfSnap.byPid.end() ? p->second.bytes : 0;
        t->cpu = p != g.perfSnap.byPid.end() ? p->second.cpu : 0;
    }
}

static void PushPerf() {
    if (!g.panel || !g.panelOpen) return;
    auto mb = [](unsigned long long b) { return std::to_wstring((long long)(b / (1024 * 1024))); };
    std::map<DWORD, int> users;   // tabs per process
    for (auto& t : g.tabs) if (t->pid) users[t->pid]++;
    unsigned long long tabBytes = 0;
    for (auto& u : users) { auto p = g.perfSnap.byPid.find(u.first); if (p != g.perfSnap.byPid.end()) tabBytes += p->second.bytes; }
    unsigned long long engine = g.perfTotal > g.perfBrowser + tabBytes ? g.perfTotal - g.perfBrowser - tabBytes : 0;
    wchar_t cpu[32]; swprintf_s(cpu, L"%.1f", g.perfCpu);
    std::wstring j = L"{\"total\":" + mb(g.perfTotal) + L",\"browser\":" + mb(g.perfBrowser) + L",\"engine\":" + mb(engine) +
                     L",\"cpu\":" + cpu + L",\"limit\":" + std::to_wstring(g.ramLimitMB) +
                     L",\"bg\":" + (g.bgLimit ? L"true" : L"false") +
                     L",\"net\":" + std::to_wstring(g.netLimitKB) + L",\"netAll\":" + (g.netLimitAll ? L"true" : L"false") +
                     L",\"auto\":" + std::to_wstring(g.autoUnloaded) +
                     L",\"trimmed\":" + std::to_wstring(g.lastTrimCount) + L",\"tabs\":[";
    for (size_t i = 0; i < g.tabs.size(); i++) {
        Tab* t = g.tabs[i].get();
        wchar_t c[32]; swprintf_s(c, L"%.1f", t->cpu);
        if (i) j += L",";
        j += L"{\"id\":" + std::to_wstring(t->id) + L",\"title\":" + JsonStr(t->title.empty() ? DisplayUrl(t) : t->title) +
             L",\"mem\":" + mb(t->memBytes) + L",\"cpu\":" + c + L",\"active\":" + (t->id == g.active ? L"true" : L"false") +
             L",\"discarded\":" + (t->discarded ? L"true" : L"false") +
             L",\"shared\":" + (t->pid && users[t->pid] > 1 ? L"true" : L"false") +
             L",\"audible\":" + (t->audible ? L"true" : L"false") + L",\"muted\":" + (t->muted ? L"true" : L"false") +
             L",\"form\":" + (t->dirty ? L"true" : L"false") +
             L",\"kept\":" + (!HostOf(t).empty() && g.keepHosts.count(HostOf(t)) ? L"true" : L"false") +
             L",\"site\":" + JsonStr(HostOf(t)) + L"}";
    }
    j += L"],\"blocked\":" + std::to_wstring(g.sleepBlocked) + L",\"keep\":[";
    bool firstKeep = true;
    for (const auto& h : g.keepHosts) { if (!firstKeep) j += L","; firstKeep = false; j += JsonStr(h); }
    j += L"]}";
    g.panel->GetMainFrame()->ExecuteJavaScript(L"window.nnPerf&&window.nnPerf(" + j + L")", PANEL_UI, 0);
}

// Priority of the renderer processes: low for background tabs while the limiter is on.
static void ApplyBackgroundLimit() {
    static std::map<DWORD, bool> applied;   // pid -> low priority is set
    std::set<DWORD> inUse;
    if (Tab* a = Active()) if (a->pid) inUse.insert(a->pid);   // the selected tab's process is never slowed down
    std::set<DWORD> live;
    for (auto& t : g.tabs) {
        if (!t->pid) continue;
        live.insert(t->pid);
        bool low = g.bgLimit && !inUse.count(t->pid);
        auto it = applied.find(t->pid);
        if ((it == applied.end() && !low) || (it != applied.end() && it->second == low)) continue;
        if (perf::SetBackground(t->pid, low)) applied[t->pid] = low;
    }
    for (auto it = applied.begin(); it != applied.end();) it = live.count(it->first) ? std::next(it) : applied.erase(it);
}

// Download speed cap per tab through DevTools network conditions (all tabs or background tabs only).
static void ApplyNetworkLimit() {
    if (g.closing) return;
    for (auto& t : g.tabs) {
        if (!t->browser || t->discarded) continue;
        bool cap = g.netLimitKB > 0 && (g.netLimitAll || t->id != g.active);
        int want = cap ? (int)g.netLimitKB : -1;
        if (t->netLimitKB == want) continue;
        if (!t->netEnabled) {
            if (want < 0) continue;
            t->browser->GetHost()->ExecuteDevToolsMethod(0, "Network.enable", CefDictionaryValue::Create());
            t->netEnabled = true;
        }
        CefRefPtr<CefDictionaryValue> p = CefDictionaryValue::Create();
        p->SetBool("offline", false);
        p->SetDouble("latency", 0.0);
        p->SetDouble("downloadThroughput", want > 0 ? (double)want * 1024.0 : -1.0);
        p->SetDouble("uploadThroughput", -1.0);
        t->browser->GetHost()->ExecuteDevToolsMethod(0, "Network.emulateNetworkConditions", p);
        t->netLimitKB = want;
    }
}

static bool UnloadTab(int id) {
    Tab* t = FindTab(id);
    if (!t || !t->browser || t->discarded || id == g.active) return false;
    t->discarding = true;
    HWND w = t->browser->GetHost()->GetWindowHandle();
    if (w) ShowWindow(w, SW_HIDE);
    t->browser->GetHost()->CloseBrowser(true);
    return true;
}

// The site of a tab for the keep-awake list: host without "www.", lower case; empty for NeonNet's own pages.
static std::wstring HostOf(Tab* t) {
    std::wstring u = DisplayUrl(t);
    if (u.rfind(L"http://", 0) != 0 && u.rfind(L"https://", 0) != 0) return {};
    if (u.rfind(UI_HOST, 0) == 0 || u.rfind(CHROME_UI_HOST, 0) == 0) return {};
    size_t s = u.find(L"://") + 3, e = u.find_first_of(L"/?#", s);
    std::wstring h = u.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
    size_t at = h.rfind(L'@'); if (at != std::wstring::npos) h.erase(0, at + 1);
    size_t colon = h.find(L':'); if (colon != std::wstring::npos) h.resize(colon);
    for (auto& c : h) c = (wchar_t)towlower(c);
    if (h.rfind(L"www.", 0) == 0) h.erase(0, 4);
    return h;
}

// Why a background tab must not go to sleep on its own, or null: sound, typed text, a site kept awake.
// (The selected and the loading tab are handled by the callers.)
static const wchar_t* SleepProtection(Tab* t) {
    if (GetTickCount64() < t->audioUntil) return L"audio";
    if (t->dirty) return L"form";
    std::wstring h = HostOf(t);
    if (!h.empty() && g.keepHosts.count(h)) return L"kept";
    return nullptr;
}

static void SaveKeepHosts() {
    std::wstring s;
    for (const auto& h : g.keepHosts) { if (!s.empty()) s += L';'; s += h; }
    if (s.empty()) RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"KeepAwakeHosts");
    else RegSetBin(L"KeepAwakeHosts", REG_SZ, s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
}

static void LoadKeepHosts() {
    std::wstring s = RegGetString(L"KeepAwakeHosts");
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(L';', i); if (e == std::wstring::npos) e = s.size();
        std::wstring h = s.substr(i, e - i);
        if (!h.empty() && h.size() < 256) g.keepHosts.insert(h);
        i = e + 1;
    }
}

static void ToggleKeepAwake(int id) {
    Tab* t = FindTab(id);
    if (!t) return;
    std::wstring h = HostOf(t);
    if (h.empty()) return;
    if (!g.keepHosts.erase(h)) g.keepHosts.insert(h);
    SaveKeepHosts();
}

static void ToggleMute(int id) {
    Tab* t = FindTab(id);
    if (!t) return;
    t->muted = !t->muted;
    if (t->browser) t->browser->GetHost()->SetAudioMuted(t->muted);
    SendState();
}

static void ApplyRamLimit() {
    if (!g.ramLimitMB || g.closing) return;
    g.sleepBlocked = 0;
    if (g.perfTotal <= (unsigned long long)g.ramLimitMB * 1024 * 1024) return;
    if (GetTickCount64() - g.lastLimitAct < 5000) return;   // give the last unload time to show in the figures
    Tab* oldest = nullptr;
    for (auto& t : g.tabs) {
        if (t->id == g.active || !t->browser || t->discarded || t->loading) continue;
        if (SleepProtection(t.get())) { g.sleepBlocked++; continue; }
        if (!oldest || t->lastActive < oldest->lastActive) oldest = t.get();
    }
    if (!oldest) return;
    std::wstring name = oldest->title.empty() ? DisplayUrl(oldest) : oldest->title;
    if (UnloadTab(oldest->id)) {
        g.autoUnloaded++;
        g.lastLimitAct = GetTickCount64();
        ShowNotice(L"RAM LIMIT: PUT TO SLEEP " + Upper(name.substr(0, 40)));
    }
}

// Once a second: which tabs make sound. The tab strip and the panel show it; a change is sent to both.
static void OnAudioTimer() {
    if (g.closing) return;
    bool changed = false;
    for (auto& t : g.tabs) {
        bool a = t->browser && !t->muted && GetTickCount64() < t->audioUntil;   // a muted tab makes no sound, but is still "playing" (see SleepProtection)
        if (a != t->audible) { t->audible = a; changed = true; }
    }
    if (changed) { SendState(); PushPerf(); }
}

static void OnPerfTimer() {
    if (g.closing) return;
    if (Tab* a = Active()) a->lastActive = GetTickCount64();
    SamplePerf();
    ApplyBackgroundLimit();
    ApplyRamLimit();
    PushPerf();
}

// First every page collects its garbage (the DevTools command), then, a moment later, the
// working sets are trimmed (TIMER_TRIM) and the figures are taken again.
static void CleanMemory() {
    for (auto& t : g.tabs)
        if (t->browser) t->browser->GetHost()->ExecuteDevToolsMethod(0, "HeapProfiler.collectGarbage", CefDictionaryValue::Create());
    if (g.hwnd) SetTimer(g.hwnd, TIMER_TRIM, 1200, nullptr);
}

static void SetPanelOpen(bool open) {
    if (open == g.panelOpen) return;
    g.panelOpen = open;
    if (open) {
        if (!g.panel) {
            g.creatingPanel = true;
            CefBrowserHost::CreateBrowserSync(ChildInfo(PanelRect()), g_client.get(), PANEL_UI, BrowserSettings(), nullptr, nullptr);
            g.creatingPanel = false;
        }
        SamplePerf();
    } else if (g.panel) {
        // closed for good, so the panel itself holds no memory while it is hidden
        CefRefPtr<CefBrowser> p = g.panel;
        g.panel = nullptr;
        p->GetHost()->CloseBrowser(true);
    }
    Layout();
    UpdatePerfTimer();
    SendState();
}

static void SetRamLimit(DWORD mb) {
    g.ramLimitMB = (mb >= 512 && mb <= 65536) ? mb : 0;
    RegSetBin(L"RamLimitMB", REG_DWORD, &g.ramLimitMB, sizeof(DWORD));
    UpdatePerfTimer();
}

static void SetBgLimit(bool on) {
    g.bgLimit = on;
    DWORD v = on ? 1 : 0;
    RegSetBin(L"BgLimit", REG_DWORD, &v, sizeof(v));
    ApplyBackgroundLimit();
    UpdatePerfTimer();
}

static void SetNetLimit(DWORD kb) {
    g.netLimitKB = (kb >= 64 && kb <= 102400) ? kb : 0;
    RegSetBin(L"NetLimitKB", REG_DWORD, &g.netLimitKB, sizeof(DWORD));
    ApplyNetworkLimit();
}

static void SetNetAll(bool all) {
    g.netLimitAll = all;
    DWORD v = all ? 1 : 0;
    RegSetBin(L"NetLimitAll", REG_DWORD, &v, sizeof(v));
    ApplyNetworkLimit();
}

// Commands from the panel page (see OnChromeCommand).
static void OnPerfCommand(const std::wstring& cmd, const std::wstring& arg) {
    int id = _wtoi(arg.c_str());
    if (cmd == L"perf.clean") CleanMemory();
    else if (cmd == L"perf.unload") { UnloadTab(id); }
    else if (cmd == L"perf.close") { if (FindTab(id)) CloseTab(id); }
    else if (cmd == L"perf.unloadbg") {
        // all background tabs except those that must stay awake (sound, typed text, kept-awake sites)
        std::vector<int> ids; int kept = 0;
        for (auto& t : g.tabs) {
            if (t->id == g.active || !t->browser || t->discarded) continue;
            if (SleepProtection(t.get())) kept++; else ids.push_back(t->id);
        }
        for (int i : ids) UnloadTab(i);
        if (kept) ShowNotice(std::to_wstring(kept) + L" TAB" + (kept == 1 ? L"" : L"S") + L" KEPT AWAKE (SOUND, TYPED TEXT OR KEEP-AWAKE SITE)");
    }
    else if (cmd == L"perf.mute") ToggleMute(id);
    else if (cmd == L"perf.keep") ToggleKeepAwake(id);
    else if (cmd == L"perf.unkeep") { g.keepHosts.erase(arg); SaveKeepHosts(); }
    else if (cmd == L"perf.ram") SetRamLimit((DWORD)_wtoi(arg.c_str()));
    else if (cmd == L"perf.bg") SetBgLimit(arg == L"1");
    else if (cmd == L"perf.net") SetNetLimit((DWORD)_wtoi(arg.c_str()));
    else if (cmd == L"perf.netall") SetNetAll(arg == L"1");
    else if (cmd == L"perf.hide") SetPanelOpen(false);
    SamplePerf();
    PushPerf();
}

static void ExecCmd(int cmd) {
    Tab* a = Active();
    CefRefPtr<CefBrowser> b = a ? a->browser : nullptr;
    switch (cmd) {
    case CMD_NEWTAB: CreateTab(NEWTAB, true); break;
    case CMD_INCOGNITO: if (g.incognito) CreateTab(NEWTAB, true); else LaunchIncognito(L""); break;
    case CMD_PERFORMANCE: SetPanelOpen(!g.panelOpen); break;
    case CMD_CLOSETAB: if (a) CloseTab(a->id); break;
    case CMD_HOME: if (b) b->GetMainFrame()->LoadURL(HomeUrl()); break;
    case CMD_RELOAD:
        if (b) { if (!a->errorUrl.empty()) b->GetMainFrame()->LoadURL(a->errorUrl); else b->Reload(); }
        break;
    case CMD_STOP: if (b) b->StopLoad(); break;
    case CMD_BACK: if (b && b->CanGoBack()) b->GoBack(); break;
    case CMD_FWD: if (b && b->CanGoForward()) b->GoForward(); break;
    case CMD_FOCUSURL: FocusUrl(); break;
    case CMD_PRINT: if (b) b->GetHost()->Print(); break;
    case CMD_NEXTTAB: case CMD_PREVTAB: {
        std::vector<int> vis;
        int cur = -1;
        for (auto& t : g.tabs) if (t->wsId == g.activeWs) {
            if (t->id == g.active) cur = (int)vis.size();
            vis.push_back(t->id);
        }
        int n = (int)vis.size();
        if (n > 1 && cur >= 0) SelectTab(vis[(cur + (cmd == CMD_NEXTTAB ? 1 : n - 1)) % n], true);
        break;
    }
    case CMD_ZOOMIN: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_IN); break;
    case CMD_ZOOMOUT: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_OUT); break;
    case CMD_ZOOMRESET: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_RESET); break;
    case CMD_BLOCKADS: {
        bool on = !g.blockAds.load();
        if (on) LoadBlocklist();   // about 82 000 names: only read into memory when blocking is wanted
        g.blockAds = on;
        DWORD v = on ? 1 : 0;
        RegSetBin(L"BlockAds", REG_DWORD, &v, sizeof(v));
        if (g.chrome) ShowAlert(on ? L"TRACKERS AND ADS BLOCKED - RELOAD A PAGE TO SEE THE EFFECT" : L"TRACKER AND AD BLOCKING IS OFF");
        break;
    }
    case CMD_SCAN: {
        g.scan = !g.scan;
        DWORD v = g.scan ? 1 : 0;
        RegSetBin(L"Scanlines", REG_DWORD, &v, sizeof(v));
        for (auto& t : g.tabs) ApplyScan(t->browser);
        SendState();
        InvalidateStatus();
        break;
    }
    case CMD_FULLSCREEN: g.fsUser = !g.fullscreen; if (!g.fsUser) g.fsElement = false; UpdateFullscreen(); break;
    case CMD_DEVTOOLS:
        if (Tab* t = Active()) {
            if (t->dev) { t->browser->GetHost()->CloseDevTools(); FocusPage(); }   // F12 again closes them
            else OpenDevTools(t, CefPoint());
        }
        break;
    case CMD_ABOUT: ShowAbout(); break;
    case CMD_HELP: CreateTab(DOCS_URL, true); break;
    case CMD_UPDATE_CHECK: StartUpdateCheck(true); break;
    case CMD_UPDATE_AUTO: {
        DWORD v = AutoUpdateEnabled() ? 0 : 1;
        RegSetBin(L"AutoUpdate", REG_DWORD, &v, sizeof(v));
        ShowNotice(v ? L"AUTOMATIC UPDATES ON" : L"AUTOMATIC UPDATES OFF");
        break;
    }
    case CMD_UPDATE_RESTART: g.updRelaunch = true; PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    case CMD_UPDATE_PAGE: if (!g.updPage.empty()) CreateTab(g.updPage, true); break;
    case CMD_FIND: OpenFind(); break;
    case CMD_FINDNEXT: case CMD_FINDPREV:
        if (!g.findOpen) OpenFind();
        else DoFind(cmd == CMD_FINDNEXT);
        break;
    case CMD_REOPEN:
        if (!g.closed.empty()) {
            ClosedTab c = g.closed.back();
            g.closed.pop_back();
            if (FindWs(c.wsId) && c.wsId != g.activeWs) SwitchWorkspace(c.wsId);
            CreateTab(c.url, true, c.index);
            if (c.pinned) SetPinned(g.active, true);
        }
        break;
    case CMD_BOOKMARK: ToggleBookmark(); break;
    case CMD_BOOKMARKS: OpenPage(BOOKMARKS_PAGE); break;
    case CMD_BOOKMARKS_BAR: {
        g.bookmarksBar = !g.bookmarksBar;
        g.bmBarDirty = true;
        DWORD v = g.bookmarksBar ? 1 : 0;
        RegSetBin(L"BookmarksBar", REG_DWORD, &v, sizeof(v));
        Layout();
        SendState();
        break;
    }
    case CMD_HISTORY: OpenPage(HISTORY_PAGE); break;
    case CMD_DOWNLOADS: OpenPage(DOWNLOADS_PAGE); break;
    case CMD_EXTENSIONS: OpenPage(EXTENSIONS_PAGE); break;
    case CMD_SETTINGS: OpenPage(SETTINGS_PAGE); break;
    }
    // zoom changes are async; refresh the status bar shortly after
    if (cmd == CMD_ZOOMIN || cmd == CMD_ZOOMOUT || cmd == CMD_ZOOMRESET) SetTimer(g.hwnd, TIMER_ZOOM, 150, nullptr);
}

// ---------------------------------------------------------------------------
// The ≡ menu: a popup window painted in the NeonNet theme (a native menu cannot be
// recolored or given clipped corners). RunMenu() runs its own message loop, like
// TrackPopupMenu, and returns the chosen command or 0.

struct MenuState {
    std::vector<MenuItem> items;
    std::vector<RECT> rows;
    HFONT font = nullptr;
    int hot = -1, result = 0;
    bool done = false, tracking = false;
    std::wstring title = L"SYSTEM MENU";
};

static const COLORREF C_MENU_BG = RGB(0x0a, 0x04, 0x20);
static const COLORREF C_TEXT = RGB(0xe8, 0xd8, 0xff);
static const COLORREF C_DIM = RGB(0x4d, 0x3a, 0x73);
static const COLORREF C_SEP = RGB(0x5c, 0x1c, 0x5e);
static const int MENU_CUT_DIP = 10;
static ULONGLONG g_menuClosedAt = 0;

static bool Selectable(const MenuState& st, int i) {
    return i >= 0 && i < (int)st.items.size() && st.items[i].cmd && !st.items[i].disabled;
}

static int MenuHit(const MenuState& st, POINT p) {
    for (size_t i = 0; i < st.rows.size(); i++)
        if (PtInRect(&st.rows[i], p) && Selectable(st, (int)i)) return (int)i;
    return -1;
}

// Thin dark lines over the window, like the CRT effect of the rest of the UI.
static void PaintScanlines(HDC mem, int w, int h) {
    if (g.scan) {
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP dot = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dot && bits) {
            *(DWORD*)bits = 0x2E000000;   // black, alpha 0.18 (premultiplied)
            HDC dd = CreateCompatibleDC(mem);
            HGDIOBJ od = SelectObject(dd, dot);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            int step = std::max(3, S(3));
            for (int y = 2; y < h; y += step) GdiAlphaBlend(mem, 0, y, w, 1, dd, 0, 0, 1, 1, bf);
            SelectObject(dd, od); DeleteDC(dd);
        }
        if (dot) DeleteObject(dot);
    }
}

static void PaintMenu(HWND hwnd, MenuState& st, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom, c = S(MENU_CUT_DIP);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(C_MENU_BG); FillRect(mem, &rc, bg); DeleteObject(bg);
    SelectObject(mem, st.font);
    SetBkMode(mem, TRANSPARENT);

    // header
    RECT hr{ S(14), S(6), w - S(14), S(26) };
    SetTextColor(mem, C_YELLOW);
    std::wstring head = L"> " + st.title;
    DrawTextW(mem, head.c_str(), -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    for (size_t i = 0; i < st.items.size(); i++) {
        const MenuItem& it = st.items[i];
        RECT r = st.rows[i];
        if (!it.cmd) {
            HPEN pen = CreatePen(PS_SOLID, 1, C_SEP);
            HGDIOBJ op = SelectObject(mem, pen);
            int y = (r.top + r.bottom) / 2;
            MoveToEx(mem, r.left + S(10), y, nullptr); LineTo(mem, r.right - S(10), y);
            SelectObject(mem, op); DeleteObject(pen);
            continue;
        }
        bool hot = (int)i == st.hot;
        if (hot) {
            // highlighted row with a clipped right edge, like the tabs
            RECT f{ r.left + S(4), r.top, r.right - S(4), r.bottom };
            int k = S(7);
            POINT pts[5] = { {f.left, f.top}, {f.right - k, f.top}, {f.right, f.top + k}, {f.right, f.bottom}, {f.left, f.bottom} };
            HBRUSH hb = CreateSolidBrush(C_CYAN);
            HRGN rg = CreatePolygonRgn(pts, 5, WINDING);
            FillRgn(mem, rg, hb);
            DeleteObject(rg); DeleteObject(hb);
        }
        COLORREF tc = it.disabled ? C_DIM : hot ? C_BG : C_TEXT;
        COLORREF kc = it.disabled ? C_DIM : hot ? C_BG : C_MUTED;
        if (it.checked) {
            RECT cr{ r.left + S(12), r.top, r.left + S(30), r.bottom };
            SetTextColor(mem, hot ? C_BG : Accent());
            DrawTextW(mem, L"\x25A0", -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        RECT tr{ r.left + S(32), r.top, r.right - S(16), r.bottom };
        SetTextColor(mem, tc);
        DrawTextW(mem, it.text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(mem, kc);
        DrawTextW(mem, it.keys.c_str(), -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    PaintScanlines(mem, w, h);

    // frame: pink outline following the clipped corners (top-right and bottom-left)
    HPEN pink = CreatePen(PS_SOLID, 1, Accent());
    HGDIOBJ op = SelectObject(mem, pink);
    POINT pts[7] = { {0, 0}, {w - c - 1, 0}, {w - 1, c}, {w - 1, h - 1}, {c, h - 1}, {0, h - c - 1}, {0, 0} };
    Polyline(mem, pts, 7);
    SelectObject(mem, op); DeleteObject(pink);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
}

static void MenuMoveHot(HWND hwnd, MenuState& st, int dir) {
    int n = (int)st.items.size();
    int i = st.hot;
    for (int k = 0; k < n; k++) {
        i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (Selectable(st, i)) { st.hot = i; break; }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (MenuState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        if (st) PaintMenu(hwnd, *st, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!st) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            st->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        int hot = MenuHit(*st, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (hot != st->hot) { st->hot = hot; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (st) { st->tracking = false; st->hot = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (st) {
            int i = MenuHit(*st, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (i >= 0) { st->result = st->items[i].cmd; st->done = true; }
        }
        return 0;
    case WM_KEYDOWN:
        if (!st) break;
        if (wp == VK_DOWN || wp == VK_TAB) MenuMoveHot(hwnd, *st, 1);
        else if (wp == VK_UP) MenuMoveHot(hwnd, *st, -1);
        else if (wp == VK_HOME) { st->hot = -1; MenuMoveHot(hwnd, *st, 1); }
        else if (wp == VK_END) { st->hot = -1; MenuMoveHot(hwnd, *st, -1); }
        else if ((wp == VK_RETURN || wp == VK_SPACE) && Selectable(*st, st->hot)) { st->result = st->items[st->hot].cmd; st->done = true; }
        else if (wp == VK_ESCAPE || wp == VK_MENU || wp == VK_F10) st->done = true;
        return 0;
    case WM_SYSKEYDOWN:
        if (st) st->done = true;
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && st) st->done = true;   // clicked somewhere else
        return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// rightTop: screen point where the menu's top-right corner goes (the ≡ menu), or with
// anchorLeft its top-left corner (the right-click menu).
static int RunMenu(std::vector<MenuItem> items, POINT rightTop, const wchar_t* title = L"SYSTEM MENU", bool anchorLeft = false) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = g.hmod;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"NeonNetMenu";
        registered = RegisterClassExW(&wc) != 0;
    }
    MenuState st;
    st.items = std::move(items);
    st.title = title;
    st.font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    // measure
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ of = SelectObject(dc, st.font);
    int textW = 0, keysW = 0;
    for (auto& it : st.items) {
        SIZE s;
        if (GetTextExtentPoint32W(dc, it.text.c_str(), (int)it.text.size(), &s)) textW = std::max(textW, (int)s.cx);
        if (!it.keys.empty() && GetTextExtentPoint32W(dc, it.keys.c_str(), (int)it.keys.size(), &s)) keysW = std::max(keysW, (int)s.cx);
    }
    SelectObject(dc, of);
    ReleaseDC(g.hwnd, dc);
    int w = S(32) + textW + S(40) + keysW + S(16);
    int y = S(30);
    for (auto& it : st.items) {
        int rh = it.cmd ? S(28) : S(11);
        st.rows.push_back({ 0, y, w, y + rh });
        y += rh;
    }
    int h = y + S(8);

    // keep it on the monitor
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(rightTop, MONITOR_DEFAULTTONEAREST), &mi);
    int x = anchorLeft ? std::max((int)mi.rcWork.left, (int)std::min(rightTop.x, mi.rcWork.right - w))
                       : std::max((int)mi.rcWork.left, (int)std::min(rightTop.x, mi.rcWork.right) - w);
    int top = std::max((int)mi.rcWork.top, (int)std::min(rightTop.y, mi.rcWork.bottom - h));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"NeonNetMenu", L"", WS_POPUP,
                                x, top, w, h, g.hwnd, nullptr, g.hmod, &st);
    if (!hwnd) { DeleteObject(st.font); return 0; }
    int c = S(MENU_CUT_DIP);
    POINT rp[6] = { {0, 0}, {w - c, 0}, {w, c}, {w, h}, {c, h}, {0, h - c} };
    SetWindowRgn(hwnd, CreatePolygonRgn(rp, 6, WINDING), FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG m;
    while (!st.done && !g.closing) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)m.wParam); break; }
        // keys typed while the menu is open belong to the menu
        if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && m.hwnd != hwnd) m.hwnd = hwnd;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    DeleteObject(st.font);
    g_menuClosedAt = GetTickCount64();
    if (!g.closing) SetForegroundWindow(g.hwnd);
    return st.result;
}

// ---------------------------------------------------------------------------
// Dialogs in the NeonNet theme: page alerts, permission prompts, sign-in.
// One window type: a title, a message, optional text boxes and buttons, drawn like the menus.

struct DlgField { std::wstring label, value; HWND edit = nullptr; RECT labelR{}, boxR{}; };
struct DlgButton { std::wstring text; int id = 0; RECT r{}; };
struct DlgState {
    std::wstring title, message;
    std::vector<DlgField> fields;
    std::vector<DlgButton> buttons;   // left to right
    int defId = 0, cancelId = 0;      // answers for Enter and Esc
    HFONT font = nullptr;
    HBRUSH editBrush = nullptr;
    RECT msgR{};
    int hot = -1, result = 0;
    bool done = false, tracking = false;
};

static void PaintDialog(HWND hwnd, DlgState& st, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom, c = S(MENU_CUT_DIP);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(C_MENU_BG); FillRect(mem, &rc, bg); DeleteObject(bg);
    SelectObject(mem, st.font);
    SetBkMode(mem, TRANSPARENT);

    RECT hr{ S(18), S(8), w - S(18), S(30) };
    SetTextColor(mem, C_YELLOW);
    std::wstring head = L"> " + st.title;
    DrawTextW(mem, head.c_str(), -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SetTextColor(mem, C_TEXT);
    DrawTextW(mem, st.message.c_str(), -1, &st.msgR, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    for (auto& f : st.fields) {
        SetTextColor(mem, C_MUTED);
        DrawTextW(mem, f.label.c_str(), -1, &f.labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        HBRUSH eb = CreateSolidBrush(C_BG); FillRect(mem, &f.boxR, eb); DeleteObject(eb);
        HPEN pen = CreatePen(PS_SOLID, 1, C_CYAN);
        HGDIOBJ op = SelectObject(mem, pen);
        HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, f.boxR.left, f.boxR.top, f.boxR.right, f.boxR.bottom);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(pen);
    }

    for (size_t i = 0; i < st.buttons.size(); i++) {
        const DlgButton& b = st.buttons[i];
        bool hot = (int)i == st.hot, def = b.id == st.defId;
        int k = S(7);
        POINT pts[5] = { {b.r.left, b.r.top}, {b.r.right - k, b.r.top}, {b.r.right, b.r.top + k}, {b.r.right, b.r.bottom}, {b.r.left, b.r.bottom} };
        HRGN rg = CreatePolygonRgn(pts, 5, WINDING);
        HBRUSH fill = CreateSolidBrush(hot ? C_CYAN : def ? Accent() : C_MENU_BG);
        FillRgn(mem, rg, fill);
        if (!hot && !def) { HBRUSH fr = CreateSolidBrush(C_MUTED); FrameRgn(mem, rg, fr, 1, 1); DeleteObject(fr); }
        DeleteObject(fill); DeleteObject(rg);
        SetTextColor(mem, hot || def ? C_BG : C_TEXT);
        RECT tr = b.r;
        DrawTextW(mem, b.text.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    PaintScanlines(mem, w, h);
    HPEN pink = CreatePen(PS_SOLID, 1, Accent());
    HGDIOBJ op = SelectObject(mem, pink);
    POINT pts[7] = { {0, 0}, {w - c - 1, 0}, {w - 1, c}, {w - 1, h - 1}, {c, h - 1}, {0, h - c - 1}, {0, 0} };
    Polyline(mem, pts, 7);
    SelectObject(mem, op); DeleteObject(pink);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
}

static int DlgButtonAt(const DlgState& st, POINT p) {
    for (size_t i = 0; i < st.buttons.size(); i++) if (PtInRect(&st.buttons[i].r, p)) return (int)i;
    return -1;
}

static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        if (st) PaintDialog(hwnd, *st, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        if (!st) break;
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_CYAN); SetBkColor(dc, C_BG);
        return (LRESULT)st->editBrush;
    }
    case WM_MOUSEMOVE: {
        if (!st) break;
        if (!st->tracking) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            st->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        int hot = DlgButtonAt(*st, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (hot != st->hot) { st->hot = hot; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (st) { st->tracking = false; st->hot = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (st) {
            int i = DlgButtonAt(*st, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (i >= 0) { st->result = st->buttons[i].id; st->done = true; }
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows the dialog and runs a message loop until it is answered. Returns the id of the button
// pressed (or cancelId for Esc, or when NeonNet is closing); the text boxes are read back into st.
static int RunDialog(DlgState& st) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = DialogProc;
        wc.hInstance = g.hmod;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"NeonNetDialog";
        registered = RegisterClassExW(&wc) != 0;
    }
    st.font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    st.editBrush = CreateSolidBrush(C_BG);

    // layout
    int W = S(480), pad = S(18);
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ of = SelectObject(dc, st.font);
    RECT mr{ 0, 0, W - 2 * pad, 0 };
    if (!st.message.empty()) DrawTextW(dc, st.message.c_str(), -1, &mr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int mh = std::min((int)mr.bottom, S(260));
    int y = S(40);
    st.msgR = { pad, y, W - pad, y + mh };
    y += mh + (mh ? S(14) : 0);
    for (auto& f : st.fields) {
        if (!f.label.empty()) { f.labelR = { pad, y, W - pad, y + S(18) }; y += S(20); }
        f.boxR = { pad, y, W - pad, y + S(30) };
        y += S(40);
    }
    y += S(4);
    int bh = S(32), gap = S(10), total = 0;
    for (auto& b : st.buttons) {
        SIZE sz{}; GetTextExtentPoint32W(dc, b.text.c_str(), (int)b.text.size(), &sz);
        b.r = { 0, y, std::max(S(96), (int)sz.cx + S(40)), y + bh };
        total += b.r.right + gap;
    }
    SelectObject(dc, of);
    ReleaseDC(g.hwnd, dc);
    int x = W - pad - (total - gap);
    for (auto& b : st.buttons) { int bw = b.r.right; b.r.left = x; b.r.right = x + bw; x += bw + gap; }
    int H = y + bh + S(20);

    // centred over the window
    RECT wr{}; GetWindowRect(g.hwnd, &wr);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int px = std::max((int)mi.rcWork.left, std::min((int)((wr.left + wr.right - W) / 2), (int)mi.rcWork.right - W));
    int py = std::max((int)mi.rcWork.top, std::min((int)((wr.top + wr.bottom - H) / 3), (int)mi.rcWork.bottom - H));

    EnableWindow(g.hwnd, FALSE);   // modal: the window behind does nothing until the dialog is answered
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"NeonNetDialog", L"", WS_POPUP, px, py, W, H, g.hwnd, nullptr, g.hmod, &st);
    if (!hwnd) { EnableWindow(g.hwnd, TRUE); DeleteObject(st.font); DeleteObject(st.editBrush); return st.cancelId; }
    int cut = S(MENU_CUT_DIP);
    POINT rp[6] = { {0, 0}, {W - cut, 0}, {W, cut}, {W, H}, {cut, H}, {0, H - cut} };
    SetWindowRgn(hwnd, CreatePolygonRgn(rp, 6, WINDING), FALSE);
    for (auto& f : st.fields) {
        f.edit = CreateWindowExW(0, L"EDIT", f.value.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 f.boxR.left + S(8), f.boxR.top + S(6), f.boxR.right - f.boxR.left - S(16), S(19), hwnd, nullptr, g.hmod, nullptr);
        SendMessageW(f.edit, WM_SETFONT, (WPARAM)st.font, TRUE);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (!st.fields.empty()) { SetFocus(st.fields[0].edit); SendMessageW(st.fields[0].edit, EM_SETSEL, 0, -1); }
    else SetFocus(hwnd);

    MSG m;
    while (!st.done && !g.closing) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)m.wParam); break; }
        if (m.message == WM_KEYDOWN) {
            if (m.wParam == VK_RETURN) { st.result = st.defId; st.done = true; continue; }
            if (m.wParam == VK_ESCAPE) { st.result = st.cancelId; st.done = true; continue; }
            if (m.wParam == VK_TAB && st.fields.size() > 1) {
                size_t cur = 0;
                for (size_t i = 0; i < st.fields.size(); i++) if (GetFocus() == st.fields[i].edit) cur = i;
                size_t nx = (cur + (GetKeyState(VK_SHIFT) < 0 ? st.fields.size() - 1 : 1)) % st.fields.size();
                SetFocus(st.fields[nx].edit); SendMessageW(st.fields[nx].edit, EM_SETSEL, 0, -1);
                continue;
            }
        }
        // keys typed while the dialog is open belong to the dialog
        if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && GetAncestor(m.hwnd, GA_ROOT) != hwnd)
            m.hwnd = st.fields.empty() ? hwnd : st.fields[0].edit;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!st.done) st.result = st.cancelId;
    for (auto& f : st.fields) {
        int n = GetWindowTextLengthW(f.edit);
        f.value.assign((size_t)n + 1, L'\0');
        GetWindowTextW(f.edit, &f.value[0], n + 1);
        f.value.resize((size_t)n);
    }
    EnableWindow(g.hwnd, TRUE);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    DeleteObject(st.font); DeleteObject(st.editBrush);
    if (!g.closing) SetForegroundWindow(g.hwnd);
    return st.result;
}

static std::wstring OriginHost(const std::wstring& o) {
    size_t s = o.find(L"://");
    std::wstring h = s == std::wstring::npos ? o : o.substr(s + 3);
    h = h.substr(0, h.find_first_of(L"/?#"));
    for (auto& c : h) c = (wchar_t)towupper(c);
    return h.empty() ? L"THIS PAGE" : h;
}

// What a permission request is for, one line each.
static std::wstring PermissionLines(bool media, uint32_t p) {
    std::wstring o;
    for (const auto& k : PermKeys(media, p))
        for (const auto& d : PERM_DEFS)
            if (k == d.key) { o += L"  - "; o += d.ask; o += L"\n"; }
    if (!o.empty()) o.pop_back();
    return o;
}

// Shows one request and answers its callback.
static void ShowDlgReq(DlgReq& r) {
    auto cancelAll = [&] {
        if (r.js) r.js->Continue(r.kind == DlgReq::UNLOAD, CefString());
        if (r.perm) r.perm->Continue(CEF_PERMISSION_RESULT_DISMISS);
        if (r.media) r.media->Cancel();
    };
    if (g.closing) { cancelAll(); return; }
    std::wstring origin;
    std::vector<std::wstring> keys;
    if (r.kind == DlgReq::PERM || r.kind == DlgReq::MEDIA) {
        origin = OriginOf(r.origin);
        keys = PermKeys(r.kind == DlgReq::MEDIA, r.perms);
        if (!origin.empty()) {
            // a choice made for this site before (in the site panel or by an earlier answer): no second question
            bool anyBlock = false, allAllow = true;
            for (const auto& k : keys) { int v = SpGet(origin, k); if (v == 2) anyBlock = true; if (v != 1) allAllow = false; }
            if (anyBlock || allAllow) {
                bool allow = !anyBlock;
                if (r.perm) r.perm->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
                if (r.media) { if (allow) r.media->Continue(r.perms); else r.media->Cancel(); }
                return;
            }
        }
    }
    if (r.tabId) if (Tab* t = FindTab(r.tabId)) if (t->id != g.active) SelectTab(t->id, false);   // the page that asks is shown

    DlgState st;
    std::wstring host = OriginHost(r.origin);
    switch (r.kind) {
    case DlgReq::ALERT:
        st.title = host + L" SAYS"; st.message = r.message;
        st.buttons = { { L"OK", 1 } }; st.defId = st.cancelId = 1;
        break;
    case DlgReq::CONFIRM:
        st.title = host + L" SAYS"; st.message = r.message;
        st.buttons = { { L"OK", 1 }, { L"CANCEL", 0 } }; st.defId = 1; st.cancelId = 0;
        break;
    case DlgReq::PROMPT:
        st.title = host + L" SAYS"; st.message = r.message;
        { DlgField f; f.value = r.defText; st.fields.push_back(f); }
        st.buttons = { { L"OK", 1 }, { L"CANCEL", 0 } }; st.defId = 1; st.cancelId = 0;
        break;
    case DlgReq::UNLOAD:
        st.title = host + (r.isReload ? L" - RELOAD?" : L" - LEAVE?");
        st.message = r.isReload ? L"Reload this site?\nChanges you made may not be saved." : L"Leave this site?\nChanges you made may not be saved.";
        st.buttons = { { r.isReload ? L"RELOAD" : L"LEAVE", 1 }, { L"STAY", 0 } }; st.defId = 1; st.cancelId = 0;
        break;
    case DlgReq::PWSAVE: {
        bool exists = PwHas(r.origin, r.defText);
        st.title = host + (exists ? L" - UPDATE PASSWORD?" : L" - SAVE PASSWORD?");
        st.message = (exists ? L"Replace the saved password for " : L"Save the password for ") + (r.defText.empty() ? std::wstring(L"this account") : r.defText) +
                     L" on " + r.origin + L"?" + (r.origin.rfind(L"http://", 0) == 0 ? L"\nThis site is not encrypted (http)." : L"") +
                     L"\nIt is stored encrypted with your Windows account.";
        st.buttons = { { exists ? L"UPDATE" : L"SAVE", 1 }, { L"NEVER FOR THIS SITE", 2 }, { L"NOT NOW", 0 } }; st.defId = 1; st.cancelId = 0;
        break;
    }
    case DlgReq::PERM: case DlgReq::MEDIA:
        st.title = host + L" WANTS TO";
        st.message = PermissionLines(r.kind == DlgReq::MEDIA, r.perms) +
                     (g.incognito ? L"\nYour answer is kept until this window closes." : L"\nYour answer is remembered for this site.") +
                     L"\nChange it any time from the lock icon in the address bar.";
        st.buttons = { { L"ALLOW", 1 }, { L"BLOCK", 2 } }; st.defId = 2; st.cancelId = 0;   // Enter blocks: the safe answer
        break;
    }
    int res = RunDialog(st);

    switch (r.kind) {
    case DlgReq::ALERT: case DlgReq::CONFIRM: r.js->Continue(res == 1, CefString()); break;
    case DlgReq::PROMPT: r.js->Continue(res == 1, st.fields[0].value); break;
    case DlgReq::UNLOAD: r.js->Continue(res == 1, CefString()); break;
    case DlgReq::PWSAVE:
        if (res == 1) PwStore(r.origin, r.defText, r.secret); else if (res == 2) PwNeverAdd(r.origin);
        if (!r.secret.empty()) SecureZeroMemory(&r.secret[0], r.secret.size() * sizeof(wchar_t));
        break;
    case DlgReq::PERM:
        if ((res == 1 || res == 2) && !origin.empty()) for (const auto& k : keys) SpSet(origin, k, res);
        r.perm->Continue(res == 1 ? CEF_PERMISSION_RESULT_ACCEPT : res == 2 ? CEF_PERMISSION_RESULT_DENY : CEF_PERMISSION_RESULT_DISMISS);
        SendState();
        break;
    case DlgReq::MEDIA:
        if ((res == 1 || res == 2) && !origin.empty()) for (const auto& k : keys) SpSet(origin, k, res);
        if (res == 1) r.media->Continue(r.perms); else r.media->Cancel();
        SendState();
        break;
    }
}

// Dialogs one after the other: a page that opens several (or two tabs asking at once) gets them in order.
static void OnDialogMsg(std::unique_ptr<DlgReq> r) {
    g.dlgQueue.push_back(std::move(r));
    if (g.dlgBusy) return;
    g.dlgBusy = true;
    while (!g.dlgQueue.empty()) {
        std::unique_ptr<DlgReq> next = std::move(g.dlgQueue.front());
        g.dlgQueue.pop_front();
        ShowDlgReq(*next);
    }
    g.dlgBusy = false;
}

static void ShowMenu(int cssRightGap, int cssY, double ratio) {
    // A click on ≡ while the menu is open first closes it (the menu loses focus);
    // that same click must not open it again.
    if (GetTickCount64() - g_menuClosedAt < 250) return;
    std::vector<MenuItem> m;
    auto add = [&](int cmd, std::wstring text, std::wstring keys = L"", bool disabled = false, bool checked = false) {
        MenuItem it; it.cmd = cmd; it.text = std::move(text); it.keys = std::move(keys); it.disabled = disabled; it.checked = checked;
        m.push_back(std::move(it));
    };
    auto sep = [&] { m.push_back(MenuItem()); };
    Tab* a = Active();
    bool starred = a && store::FindBookmark(DisplayUrl(a)) != 0;
    bool canStar = !g.incognito && a && store::Recordable(DisplayUrl(a));
    add(CMD_NEWTAB, L"New tab", L"Ctrl+T");
    add(CMD_CLOSETAB, L"Close tab", L"Ctrl+W");
    add(CMD_REOPEN, L"Reopen closed tab", L"Ctrl+Shift+T", g.closed.empty());
    add(CMD_INCOGNITO, g.incognito ? L"New incognito tab" : L"New incognito window", L"Ctrl+Shift+N");
    sep();
    add(CMD_BOOKMARK, starred ? L"Remove bookmark" : L"Bookmark this page", L"Ctrl+D", !canStar);
    add(CMD_BOOKMARKS, L"Bookmarks", L"Ctrl+Shift+O");
    add(CMD_BOOKMARKS_BAR, L"Bookmarks bar", L"Ctrl+Shift+B", false, g.bookmarksBar);
    add(CMD_HISTORY, L"History", L"Ctrl+H", g.incognito);   // nothing is recorded in an incognito window
    add(CMD_DOWNLOADS, L"Downloads", L"Ctrl+J");
    add(CMD_EXTENSIONS, L"Extensions (Userscripts)", L"Ctrl+Shift+E");
    add(CMD_PERFORMANCE, L"Performance panel", L"Ctrl+Shift+M", false, g.panelOpen);
    add(CMD_FIND, L"Find in page", L"Ctrl+F");
    sep();
    add(CMD_ZOOMIN, L"Zoom in", L"Ctrl++");
    add(CMD_ZOOMOUT, L"Zoom out", L"Ctrl+-");
    add(CMD_ZOOMRESET, L"Reset zoom", L"Ctrl+0");
    sep();
    add(CMD_PRINT, L"Print", L"Ctrl+P");
    add(CMD_BLOCKADS, L"Block trackers and ads", L"", false, g.blockAds.load());
    add(CMD_SCAN, L"Scanlines", L"", false, g.scan);
    add(CMD_FULLSCREEN, L"Fullscreen", L"F11", false, g.fullscreen);
    add(CMD_DEVTOOLS, L"Developer tools", L"F12");
    sep();
    add(CMD_SETTINGS, L"Settings", L"Ctrl+,");
    if (!g.incognito) {   // updates are handled by the normal window
        if (g.upd == U_READY) add(CMD_UPDATE_RESTART, L"Restart to update to v" + g.updVersion);
        if (g.upd == U_AVAILABLE) add(CMD_UPDATE_PAGE, L"Download v" + g.updVersion);
        add(CMD_UPDATE_CHECK, L"Check for updates", L"", g.upd == U_CHECKING || g.upd == U_DOWNLOADING);
        if (IsInstalledCopy()) add(CMD_UPDATE_AUTO, L"Automatic updates", L"", false, AutoUpdateEnabled());
    }
    add(CMD_HELP, L"User guide", L"F1");
    add(CMD_ABOUT, L"About NeonNet");

    RECT c = ChromeRect();
    // Right edge of the ≡ button, measured from the right edge of the UI area, which we know
    // exactly in pixels. Only the small gap and the button's bottom go through the CSS-to-pixel
    // factor (devicePixelRatio, which is what Chromium actually uses), so a wrong factor can
    // only move the menu by a few pixels instead of hundreds.
    auto px = [&](double css) { return (int)lround(css * ratio); };
    POINT p{ c.right - px(cssRightGap), c.top + px(cssY) + px(2) };
    ClientToScreen(g.hwnd, &p);
    int cmd = RunMenu(std::move(m), p);
    if (cmd) ExecCmd(cmd);
}

// Right-click on a tab in the strip. cssX/cssY: the click inside the toolbar page, in CSS pixels.
static void ShowTabMenu(int id, int cssX, int cssY, double ratio) {
    Tab* t = FindTab(id);
    if (!t || GetTickCount64() - g_menuClosedAt < 250) return;
    enum { T_PIN = 1, T_DUP, T_MUTE, T_CLOSE_OTHERS, T_CLOSE, T_NEW_WS, T_MOVE_BASE = 100 };
    std::vector<MenuItem> m;
    auto add = [&](int cmd, std::wstring text, bool disabled = false) {
        MenuItem it; it.cmd = cmd; it.text = std::move(text); it.disabled = disabled;
        m.push_back(std::move(it));
    };
    add(T_PIN, t->pinned ? L"Unpin tab" : L"Pin tab");
    add(T_DUP, L"Duplicate tab", DisplayUrl(t).empty());
    if (t->audible || t->muted) add(T_MUTE, t->muted ? L"Unmute tab" : L"Mute tab");
    m.push_back(MenuItem());
    std::vector<int> moveTargets;
    for (const auto& w : g.workspaces) {
        if (w.id == t->wsId) continue;
        int cmdId = T_MOVE_BASE + (int)moveTargets.size();
        moveTargets.push_back(w.id);
        add(cmdId, L"Move to " + w.icon + L" " + w.name);
    }
    if (g.workspaces.size() < 12) add(T_NEW_WS, L"Move to new workspace");
    m.push_back(MenuItem());
    int others = 0;
    for (auto& o : g.tabs) if (o->id != id && o->wsId == t->wsId && !o->pinned) others++;
    add(T_CLOSE_OTHERS, L"Close other tabs", others == 0);   // pinned tabs stay
    add(T_CLOSE, L"Close tab");

    RECT c = ChromeRect();
    if (!(ratio >= 0.5 && ratio <= 8)) ratio = g.dpi / 96.0;
    POINT p{ c.left + (int)lround(cssX * ratio), c.top + (int)lround(cssY * ratio) };
    ClientToScreen(g.hwnd, &p);
    int cmd = RunMenu(std::move(m), p, L"TAB", true);
    if (g.closing || !FindTab(id)) return;
    if (cmd >= T_MOVE_BASE && cmd < T_MOVE_BASE + (int)moveTargets.size()) {
        MoveTabToWorkspace(id, moveTargets[cmd - T_MOVE_BASE]);
        return;
    }
    switch (cmd) {
    case T_PIN: SetPinned(id, !FindTab(id)->pinned); break;
    case T_DUP: { Tab* s = FindTab(id); CreateTab(DisplayUrl(s), true, IndexOf(id) + 1); break; }
    case T_MUTE: ToggleMute(id); break;
    case T_NEW_WS: CreateWorkspace(L"WS " + std::to_wstring(g.nextWsId), L"\u26A1", L"#ff2e97", id); break;
    case T_CLOSE_OTHERS: {
        int ws = FindTab(id)->wsId;
        std::vector<int> ids;
        for (auto& o : g.tabs) if (o->id != id && o->wsId == ws && !o->pinned) ids.push_back(o->id);
        for (int i : ids) CloseTab(i);
        SelectTab(id, true);
        break;
    }
    case T_CLOSE: CloseTab(id); break;
    }
}

static void ShowWsMenu(int wsId, int cssX, int cssY, double ratio) {
    Workspace* w = FindWs(wsId);
    if (!w || GetTickCount64() - g_menuClosedAt < 250) return;
    enum { W_SWITCH = 1, W_EDIT, W_NEW, W_DEL };
    std::vector<MenuItem> m;
    auto add = [&](int cmd, std::wstring text, bool disabled = false) {
        MenuItem it; it.cmd = cmd; it.text = std::move(text); it.disabled = disabled;
        m.push_back(std::move(it));
    };
    if (wsId != g.activeWs) add(W_SWITCH, L"Switch to " + w->name);
    add(W_EDIT, L"Edit workspace...");
    add(W_NEW, L"New workspace...", g.workspaces.size() >= 12);
    m.push_back(MenuItem());
    add(W_DEL, L"Delete workspace", g.workspaces.size() <= 1);

    RECT c = ChromeRect();
    if (!(ratio >= 0.5 && ratio <= 8)) ratio = g.dpi / 96.0;
    POINT p{ c.left + (int)lround(cssX * ratio), c.top + (int)lround(cssY * ratio) };
    ClientToScreen(g.hwnd, &p);
    int cmd = RunMenu(std::move(m), p, L"WORKSPACE", true);
    if (g.closing) return;
    switch (cmd) {
    case W_SWITCH: SwitchWorkspace(wsId); break;
    case W_EDIT: RunInChrome(L"window.__nn&&window.__nn({type:'wsEdit',id:" + std::to_wstring(wsId) + L"})"); break;
    case W_NEW: RunInChrome(L"window.__nn&&window.__nn({type:'wsEdit',id:0})"); break;
    case W_DEL: DeleteWorkspace(wsId); break;
    }
}

// Right-click on a bookmarks bar item (or the bar itself).
static void ShowBookmarkMenu(long long id, bool isFolder, const std::wstring& url, int cssX, int cssY, double ratio) {
    if (GetTickCount64() - g_menuClosedAt < 250) return;
    enum { B_OPEN = 1, B_NEWTAB, B_DEL, B_MGR, B_HIDE };
    std::vector<MenuItem> m;
    auto add = [&](int cmd, std::wstring text, bool disabled = false) {
        MenuItem it; it.cmd = cmd; it.text = std::move(text); it.disabled = disabled;
        m.push_back(std::move(it));
    };
    if (!isFolder && store::Recordable(url)) {
        add(B_OPEN, L"Open");
        add(B_NEWTAB, L"Open in new tab");
        m.push_back(MenuItem());
    }
    if (id > 0) {
        add(B_DEL, isFolder ? L"Delete folder" : L"Delete bookmark", g.incognito);
        m.push_back(MenuItem());
    }
    add(B_MGR, L"Bookmarks manager");
    add(B_HIDE, L"Hide bookmarks bar");

    RECT c = ChromeRect();
    if (!(ratio >= 0.5 && ratio <= 8)) ratio = g.dpi / 96.0;
    POINT p{ c.left + (int)lround(cssX * ratio), c.top + (int)lround(cssY * ratio) };
    ClientToScreen(g.hwnd, &p);
    int cmd = RunMenu(std::move(m), p, L"BOOKMARK", true);
    if (g.closing) return;
    switch (cmd) {
    case B_OPEN: if (Tab* a = Active()) if (a->browser && store::Recordable(url)) { a->browser->GetMainFrame()->LoadURL(url); FocusPage(); } break;
    case B_NEWTAB: if (store::Recordable(url)) CreateTab(url, true); break;
    case B_DEL: if (!g.incognito && id > 0 && store::DeleteBookmark(id)) { g.bmBarDirty = true; ScheduleSave(); SendState(); } break;
    case B_MGR: OpenPage(BOOKMARKS_PAGE); break;
    case B_HIDE: ExecCmd(CMD_BOOKMARKS_BAR); break;
    }
}

// The right-click menu. The chosen command goes back to Chromium, which runs it (or hands it to
// OnContextMenuCommand for NeonNet's own items).
// ---------------------------------------------------------------------------
// Clear browsing data. CEF has no "delete everything since" call, so:
//  - cookies are visited and deleted by their creation time (exact);
//  - site data (local storage, IndexedDB, service workers, cache storage ...) is deleted per
//    origin through the DevTools protocol, for the sites that had a cookie removed and the sites
//    in the history for that period (a best match: the engine keeps no list of sites to ask);
//  - the HTTP cache is always cleared completely, whatever the range (it only holds copies).
// Works on the incognito session when run from an incognito window.

class CookieClearer : public CefCookieVisitor {
public:
    CookieClearer(long long cutoffMs, bool history) { s_.cutoffMs = cutoffMs; s_.history = history; }
    bool Visit(const CefCookie& c, int, int, bool& deleteCookie) override {
        long long createdMs = c.creation.val / 1000 - 11644473600000LL;   // microseconds since 1601
        if (s_.cutoffMs && createdMs < s_.cutoffMs) return true;
        deleteCookie = true;
        s_.cookies++;
        std::wstring host = CefString(&c.domain).ToWString();
        if (!host.empty() && host[0] == L'.') host.erase(0, 1);
        if (!host.empty()) s_.hosts.insert(host);
        return true;
    }
    // Released once the visit is over (also when there were no cookies): go on with the site data.
    ~CookieClearer() {
        auto* next = new ClearSites(std::move(s_));
        if (!g.hwnd || !PostMessageW(g.hwnd, WM_APP_CLEARSITES, 0, (LPARAM)next)) delete next;
    }
private:
    ClearSites s_;
    IMPLEMENT_REFCOUNTING(CookieClearer);
};

static void FinishClear(int cookies, int sites, int history) {
    g.clearCookies = cookies; g.clearSites = sites; g.clearHistory = history;
    g.clearState = 2;
}

// History from the cut-off on. Open tabs keep their (now missing) entry ids, which the store ignores.
static int ClearHistoryData(long long cutoffMs) {
    int n = store::ClearHistorySince(cutoffMs);
    if (n) { ScheduleSave(); SendState(); }
    return n;
}

static void StartClearData(std::unique_ptr<ClearReq> r) {
    CefRefPtr<CefRequestContext> ctx = g.reqCtx ? g.reqCtx : CefRequestContext::GetGlobalContext();
    if (r->cache) {
        // CefRequestContext::ClearHttpCache() crashes libcef in this (Chrome-style) build, in the
        // normal profile and in an incognito one. The DevTools command empties the cache of the
        // context of the tab it is sent to.
        for (auto& t : g.tabs) {
            if (!t->browser) continue;
            t->browser->GetHost()->ExecuteDevToolsMethod(0, "Network.clearBrowserCache", CefDictionaryValue::Create());
            break;
        }
    }
    CefRefPtr<CefCookieManager> cm = r->cookies ? ctx->GetCookieManager(nullptr) : nullptr;
    if (!cm) { FinishClear(0, 0, r->history ? ClearHistoryData(r->cutoffMs) : 0); return; }
    // the visitor's destructor posts WM_APP_CLEARSITES with the cut-off and what it removed; the
    // history is cleared there, after the sites it names have been used to find site data
    if (!cm->VisitAllCookies(new CookieClearer(r->cutoffMs, r->history))) FinishClear(0, 0, r->history ? ClearHistoryData(r->cutoffMs) : 0);
}

static void ClearSiteData(std::unique_ptr<ClearSites> s) {
    std::set<std::wstring> origins;
    for (const auto& h : s->hosts) { origins.insert(L"https://" + h); origins.insert(L"http://" + h); }
    for (const auto& o : store::OriginsSince(s->cutoffMs, 500)) origins.insert(o);
    // pages open right now (cookies carry no port, history is empty in an incognito window)
    for (auto& t : g.tabs) {
        const std::wstring& u = t->url;
        if (u.rfind(L"http://", 0) != 0 && u.rfind(L"https://", 0) != 0) continue;
        if (u.rfind(UI_HOST, 0) == 0) continue;   // NeonNet's own pages hold no site data
        size_t slash = u.find(L'/', u.find(L"://") + 3);
        origins.insert(slash == std::wstring::npos ? u : u.substr(0, slash));
    }
    CefRefPtr<CefBrowser> b;
    for (auto& t : g.tabs) if (t->browser) { b = t->browser; break; }
    int n = 0;
    if (b) {
        for (const auto& o : origins) {
            if (n >= 600) break;
            CefRefPtr<CefDictionaryValue> p = CefDictionaryValue::Create();
            p->SetString("origin", o);
            p->SetString("storageTypes", "appcache,file_systems,indexeddb,local_storage,websql,service_workers,cache_storage");
            b->GetHost()->ExecuteDevToolsMethod(0, "Storage.clearDataForOrigin", p);
            n++;
        }
    }
    FinishClear(s->cookies, n, s->history ? ClearHistoryData(s->cutoffMs) : 0);
}

static void ShowContextMenu(std::unique_ptr<CtxMenu> m) {
    if (g.closing) { m->callback->Cancel(); return; }
    if (GetTickCount64() - g_menuClosedAt < 150) { m->callback->Cancel(); return; }   // the click that closed another menu
    int cmd = RunMenu(std::move(m->items), m->at, L"CONTEXT", true);
    if (cmd > 0) m->callback->Continue(cmd, EVENTFLAG_NONE);
    else m->callback->Cancel();
}

static void OnChromeCommand(const std::wstring& msg) {
    size_t nl = msg.find(L'\n');
    std::wstring cmd = msg.substr(0, nl);
    std::wstring arg = nl == std::wstring::npos ? L"" : msg.substr(nl + 1);
    Tab* a = Active();
    if (cmd == L"ready") { g.lastStateJson.clear(); SendState(); }
    else if (cmd == L"navigate") {
        std::wstring low = arg; for (auto& ch : low) ch = towlower(ch);
        if (a && a->browser && !arg.empty() && low.rfind(L"javascript:", 0) != 0) {
            a->browser->GetMainFrame()->LoadURL(arg);
            FocusPage();
        }
    }
    else if (cmd == L"back") ExecCmd(CMD_BACK);
    else if (cmd == L"fwd") ExecCmd(CMD_FWD);
    else if (cmd == L"reload") ExecCmd(CMD_RELOAD);
    else if (cmd == L"stop") ExecCmd(CMD_STOP);
    else if (cmd == L"newtab") {
        if (!arg.empty() && store::Recordable(arg)) CreateTab(arg, true);
        else ExecCmd(CMD_NEWTAB);
    }
    else if (cmd == L"mute") ToggleMute(_wtoi(arg.c_str()));
    else if (cmd == L"perf") ExecCmd(CMD_PERFORMANCE);
    else if (cmd.rfind(L"perf.", 0) == 0) OnPerfCommand(cmd, arg);
    else if (cmd == L"home") ExecCmd(CMD_HOME);
    else if (cmd == L"dropdown") {
        // address bar suggestions: the toolbar height they need (CSS px, 0 = closed)
        // arguments: the height, then optionally where a narrower dropdown starts and ends
        int n = std::max(0, std::min(_wtoi(arg.c_str()), 520)), x0 = 0, x1 = 0;
        size_t p1 = arg.find(L'\n');
        if (p1 != std::wstring::npos) {
            size_t p2 = arg.find(L'\n', p1 + 1);
            x0 = std::max(0, _wtoi(arg.c_str() + p1 + 1));
            if (p2 != std::wstring::npos) x1 = std::max(0, _wtoi(arg.c_str() + p2 + 1));
        }
        if (n == 0) x0 = x1 = 0;
        if (n != g.dropNeed || x0 != g.dropX0 || x1 != g.dropX1) { g.dropNeed = n; g.dropX0 = x0; g.dropX1 = x1; Layout(); }
    }
    else if (cmd == L"siteperm") {
        // "<key>\n<0|1|2>" for the page that is shown (never for an address the toolbar names): 0 ask, 1 allow, 2 block;
        // key "*" forgets everything set for this site
        size_t b = arg.find(L'\n');
        std::wstring key = arg.substr(0, b), val = b == std::wstring::npos ? L"" : arg.substr(b + 1);
        std::wstring origin = a ? OriginOf(DisplayUrl(a)) : L"";
        if (SpOriginValid(origin) && origin != L"https://neonnet.ui") {
            if (key == L"*") SpReset(origin);
            else if (PermKeyValid(key) && (val == L"0" || val == L"1" || val == L"2")) SpSet(origin, key, _wtoi(val.c_str()));
            SendState();
        }
    }
    else if (cmd == L"opentab") {
        // NeonNet's own pages only (the site panel links to the permission list)
        if (arg.rfind(UI_HOST, 0) == 0 && arg.size() < 200) CreateTab(arg, true);
    }
    else if (cmd == L"close") CloseTab(_wtoi(arg.c_str()));
    else if (cmd == L"pin") { if (Tab* t = FindTab(_wtoi(arg.c_str()))) SetPinned(t->id, !t->pinned); }
    else if (cmd == L"move" || cmd == L"tabmenu" || cmd == L"bmmenu" || cmd == L"wsmenu" || cmd == L"wscreate" || cmd == L"wsupdate") {
        // arguments: the tab, then the new place (move) or the click position in CSS pixels and
        // the page's devicePixelRatio (tabmenu / bmmenu / wsmenu)
        std::vector<std::wstring> v;
        for (size_t s = 0;;) {
            size_t e = arg.find(L'\n', s);
            v.push_back(arg.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s));
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
        if (cmd == L"move" && v.size() > 1) MoveTab(_wtoi(v[0].c_str()), _wtoi(v[1].c_str()));
        else if (cmd == L"tabmenu" && v.size() > 3) ShowTabMenu(_wtoi(v[0].c_str()), _wtoi(v[1].c_str()), _wtoi(v[2].c_str()), wcstod(v[3].c_str(), nullptr));
        else if (cmd == L"bmmenu" && v.size() > 5) ShowBookmarkMenu(_wtoi64(v[0].c_str()), v[1] == L"1", v[2], _wtoi(v[3].c_str()), _wtoi(v[4].c_str()), wcstod(v[5].c_str(), nullptr));
        else if (cmd == L"wsmenu" && v.size() > 3) ShowWsMenu(_wtoi(v[0].c_str()), _wtoi(v[1].c_str()), _wtoi(v[2].c_str()), wcstod(v[3].c_str(), nullptr));
        else if (cmd == L"wscreate" && v.size() > 2) CreateWorkspace(v[0], v[1], v[2]);
        else if (cmd == L"wsupdate" && v.size() > 3) UpdateWorkspace(_wtoi(v[0].c_str()), v[1], v[2], v[3]);
    }
    else if (cmd == L"wsswitch") SwitchWorkspace(_wtoi(arg.c_str()));
    else if (cmd == L"wsdelete") DeleteWorkspace(_wtoi(arg.c_str()));
    else if (cmd == L"select") SelectTab(_wtoi(arg.c_str()), arg.find(L'\n') == std::wstring::npos);   // "<id>\nnofocus": a press that may become a drag
    else if (cmd == L"scan") ExecCmd(CMD_SCAN);
    else if (cmd == L"bookmark") ExecCmd(CMD_BOOKMARK);
    else if (cmd == L"find") { g.findText = arg.substr(0, 1000); DoFind(true); }
    else if (cmd == L"findnext") DoFind(true);
    else if (cmd == L"findprev") DoFind(false);
    else if (cmd == L"findcase") { g.findCase = !g.findCase; SendState(); DoFind(true); }
    else if (cmd == L"findclose") CloseFind();
    else if (cmd == L"focusPage") FocusPage();
    else if (cmd == L"min") ShowWindow(g.hwnd, SW_MINIMIZE);
    else if (cmd == L"max") ShowWindow(g.hwnd, IsZoomed(g.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    else if (cmd == L"quit") PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    else if (cmd == L"drag") {
        if (GetAsyncKeyState(VK_LBUTTON) < 0 && !g.fullscreen) {
            ReleaseCapture();
            SendMessageW(g.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    }
    else if (cmd == L"menu") {
        // arguments: distance from the button's right edge to the right edge of the UI and the
        // button's bottom (both CSS pixels), and the page's devicePixelRatio
        std::vector<std::wstring> v;
        for (size_t s = 0;;) {
            size_t nl = arg.find(L'\n', s);
            v.push_back(arg.substr(s, nl == std::wstring::npos ? std::wstring::npos : nl - s));
            if (nl == std::wstring::npos) break;
            s = nl + 1;
        }
        double ratio = v.size() > 2 ? wcstod(v[2].c_str(), nullptr) : 0;
        if (!(ratio >= 0.5 && ratio <= 8)) ratio = g.dpi / 96.0;
        ShowMenu(_wtoi(v[0].c_str()), v.size() > 1 ? _wtoi(v[1].c_str()) : 0, ratio);
    }
}

// ---------------------------------------------------------------------------
// Application: browser-process callbacks

static std::wstring NormalizeArgUrl(std::wstring u) {
    // neonnet://settings from the command line: the short form of NeonNet's own pages
    if (_wcsnicmp(u.c_str(), L"neonnet://", 10) == 0) {
        std::wstring name = u.substr(10), tail;
        size_t cut = name.find_first_of(L"/?#");
        if (cut != std::wstring::npos) { tail = name.substr(cut); name.resize(cut); }
        if (!tail.empty() && tail[0] == L'/') tail.erase(0, 1);
        for (auto& c : name) c = towlower(c);
        bool ok = !name.empty();
        for (wchar_t c : name) if (!(iswalnum(c) || c == L'_' || c == L'-')) ok = false;
        return ok ? std::wstring(UI_HOST) + name + L".html" + tail : std::wstring(NEWTAB);
    }
    if (u.empty() || u.find(L"://") != std::wstring::npos) return u;
    if (PathFileExistsW(u.c_str())) {
        wchar_t buf[2048]; DWORD n = 2048;
        if (SUCCEEDED(UrlCreateFromPathW(u.c_str(), buf, &n, 0))) return buf;
    }
    return L"https://" + u;
}

// Renderer side: tells the browser process when a page asks for DRM (the key system is missing from
// this build) or a video element fails with "format not supported". The page cannot call the
// reporter: it is removed from the global object by the same script that picks it up.
class MediaReporter : public CefV8Handler {
public:
    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args, CefRefPtr<CefV8Value>&, CefString&) override {
        if (args.empty() || args.size() > 2 || !args[0]->IsString()) return true;
        CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
        if (!ctx || !ctx->GetFrame()) return true;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("nn.media");
        msg->GetArgumentList()->SetString(0, args[0]->GetStringValue());
        if (args.size() == 2 && args[1]->IsString()) msg->GetArgumentList()->SetString(1, args[1]->GetStringValue());
        ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }
    IMPLEMENT_REFCOUNTING(MediaReporter);
};

static const char* MEDIA_HOOK_JS =
    "(function(){var r=window.__nn;try{delete window.__nn;}catch(e){}"
    "if(typeof r!=='function')return;var sent={};"
    "function tell(k,x){if(sent[k])return;sent[k]=1;try{if(x)r(k,x);else r(k);}catch(e){}}"
    // ck: when the user last clicked a media element. A video with preload=none only fails after it is played.
    "var ck=0;function openv(v){var s=v.querySelector('source'),u=v.currentSrc||v.src||(s&&s.src)||'';"
    "if(/^https?:\\/\\//i.test(u))try{r('openplayer',u);}catch(x){}}"
    "try{var P=Navigator.prototype,o=P.requestMediaKeySystemAccess;"
    "if(typeof o==='function'){P.requestMediaKeySystemAccess=function(ks){var p=o.apply(this,arguments);"
    "try{if(/widevine|playready|fairplay/i.test(String(ks)))p.then(null,function(){tell('drm');});}catch(e){}return p;};}}catch(e){}"
    "try{document.addEventListener('error',function(e){var t=e.target;if(!t)return;var v=null;"
    // a media element failed itself, or its last <source> failed (then the element has no error object, only networkState 3)
    "if((t.tagName==='VIDEO'||t.tagName==='AUDIO')&&t.error&&t.error.code===4)v=t;"
    "else if(t.tagName==='SOURCE'&&!t.nextElementSibling&&t.parentNode&&t.parentNode.networkState===3)v=t.parentNode;"
    "if(!v)return;if(ck&&Date.now()-ck<8000){ck=0;openv(v);return;}var ct=String(document.contentType||'');"
    // a page that is nothing but the video (a direct link to a file)
    "if(window===window.top&&/^(video|audio)\\//i.test(ct)&&v.parentNode===document.body&&document.body.childElementCount===1)tell('mediadoc',ct);"
    "else tell('media');},true);}catch(e){}"
    // Clicking (play) on a video that cannot be played opens it in the system player, no menu needed.
    // The browser process checks the address again and ignores a second click while a copy is being made.
    // The built-in play button sends no click event to the page, only 'play'. Count it when the user just interacted.
    "try{document.addEventListener('play',function(e){var t=e.target;"
    "if(t&&(t.tagName==='VIDEO'||t.tagName==='AUDIO')&&(!navigator.userActivation||navigator.userActivation.isActive))ck=Date.now();},true);}catch(e){}"
    "try{document.addEventListener('click',function(e){var t=e.target;"
    "if(!t||(t.tagName!=='VIDEO'&&t.tagName!=='AUDIO'))return;"
    "var failed=(t.error&&t.error.code===4)||(t.networkState===3&&t.readyState===0);"
    "if(failed)openv(t);else ck=Date.now();},true);}catch(e){}"
    // Sound: once a media element has played, check each second whether one really makes sound (playing, not
    // muted, volume above zero, and the decoder is still producing audio: webkitAudioDecodedByteCount grows)
    // and say so every second while it does. Sound made through the Web Audio API alone is not detected.
    "try{var tmr=0,nb=new WeakMap();function tick(){var a=false;try{var m=document.querySelectorAll('video,audio');"
    "for(var i=0;i<m.length&&i<50;i++){var e=m[i];if(e.paused||e.ended||e.muted||e.volume===0||e.readyState<3)continue;"
    "var b=e.webkitAudioDecodedByteCount,p=nb.get(e);nb.set(e,b);if(p!==undefined&&(b===undefined||b>p))a=true;}}catch(x){}"
    "if(a){try{r('audio','1');}catch(x){}}}"
    "['play','playing'].forEach(function(n){document.addEventListener(n,function(){if(!tmr)tmr=setInterval(tick,1000);},true);});}catch(e){}"
    // Typed text: report whether a text field, text area or editable area on the page holds text, so the
    // performance panel does not put the tab to sleep and lose it. Checked shortly after typing stops.
    "try{var dirty=false,dt=0;function scan(){var d=false;try{"
    "var q=document.querySelectorAll('textarea,input,[contenteditable]');"
    "for(var i=0;i<q.length&&i<300&&!d;i++){var e=q[i],tg=e.tagName,ty=String(e.type||'').toLowerCase();"
    "if(tg==='INPUT'){if(/^(hidden|checkbox|radio|button|submit|reset|image|file|range|color|search)$/.test(ty))continue;}"
    "else if(tg!=='TEXTAREA'&&!e.isContentEditable)continue;"
    "var v=tg==='INPUT'||tg==='TEXTAREA'?e.value:e.textContent;if(v&&/\\S/.test(v))d=true;}}catch(x){}"
    "if(d!==dirty){dirty=d;try{r('dirty',d?'1':'0');}catch(x){}}}"
    "['input','change'].forEach(function(n){document.addEventListener(n,function(){clearTimeout(dt);dt=setTimeout(scan,500);},true);});}catch(e){}"
    "})();";

static const char* PW_HOOK_JS = R"NNJS(
(function(){
var send=window.__nnp,reg=window.__nnpr;
try{delete window.__nnp;delete window.__nnpr;}catch(e){}
if(typeof send!=='function'||typeof reg!=='function'||window!==window.top)return;
var users=null,asked=false,focusEl=null,host=null,lastSub=0;
function vis(e){var s=e.getBoundingClientRect();return s.width>0&&s.height>0;}
function isPw(e){return !!e&&e.tagName==='INPUT'&&String(e.type).toLowerCase()==='password';}
function pwIn(scope){var q=scope.querySelectorAll('input[type=password]');for(var i=0;i<q.length;i++)if(vis(q[i]))return q[i];return null;}
function userOf(pw){var sc=pw.form||document,q=sc.querySelectorAll('input'),p=null;
for(var i=0;i<q.length;i++){var e=q[i];if(e===pw)break;var t=String(e.type||'text').toLowerCase();
if((t==='text'||t==='email'||t==='tel')&&vis(e)&&!e.disabled)p=e;}return p;}
function setv(e,v){try{var d=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value');d.set.call(e,v);
e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));}catch(x){}}
function hide(){if(host){host.remove();host=null;}}
// the list of saved user names, shown under the field the user clicked into (closed shadow DOM: the page cannot read it)
function show(list,el){hide();if(!list.length||document.activeElement!==el)return;
host=document.createElement('div');var sh=host.attachShadow({mode:'closed'});
var st=document.createElement('style');
st.textContent='div{font:13px Consolas,monospace;background:#0a0420;border:1px solid #00e5ff;color:#e8d8ff;box-shadow:0 4px 18px #000a}'
+'b{display:block;padding:3px 10px;color:#ffd600;font-weight:normal;font-size:11px;letter-spacing:1px}'
+'a{display:block;padding:7px 10px;cursor:pointer}a:hover{background:#00e5ff;color:#12062c}';
var box=document.createElement('div');var h=document.createElement('b');h.textContent='> SAVED PASSWORDS';box.appendChild(h);
list.forEach(function(u){var a=document.createElement('a');a.textContent=u||'(no user name)';
a.addEventListener('mousedown',function(e){e.preventDefault();});
a.addEventListener('click',function(e){if(!e.isTrusted)return;hide();try{send('get',u);}catch(x){}});box.appendChild(a);});
sh.appendChild(st);sh.appendChild(box);
var r=el.getBoundingClientRect();
host.style.cssText='all:initial;position:fixed;z-index:2147483647;left:'+Math.max(0,r.left)+'px;top:'+(r.bottom+2)+'px;min-width:'+Math.max(180,r.width)+'px';
(document.body||document.documentElement).appendChild(host);}
reg(function(kind,a,b){
if(kind==='list'){try{users=JSON.parse(a);}catch(x){users=[];}show(users,focusEl);}
else if(kind==='fill'){var pw=pwIn(document);if(!pw)return;var u=userOf(pw);if(u&&a)setv(u,a);setv(pw,b);}
});
document.addEventListener('focusin',function(e){var el=e.target;if(!el||el.tagName!=='INPUT')return;
var pw=isPw(el)?el:(el.form?pwIn(el.form):null);if(!pw){hide();return;}
if(!isPw(el)&&userOf(pw)!==el){hide();return;}
focusEl=el;if(users===null&&!asked){asked=true;try{send('list');}catch(x){}}else if(users)show(users,el);},true);
document.addEventListener('mousedown',function(e){if(host&&e.target!==host)hide();},true);
document.addEventListener('keydown',function(e){if(e.key==='Escape')hide();},true);
// a login being sent: remember what was typed, the browser process asks to save it once the login worked
function watch(pw){var n=0,iv=setInterval(function(){n++;if(!pw.isConnected||!vis(pw)){clearInterval(iv);try{send('ok');}catch(x){}}else if(n>10)clearInterval(iv);},1000);}
function snap(sc){var pw=sc&&pwIn(sc);if(!pw||!pw.value)return;var now=Date.now();if(now-lastSub<1500)return;lastSub=now;
var u=userOf(pw);try{send('submit',JSON.stringify({u:u?u.value:'',p:pw.value}));}catch(x){}watch(pw);}
document.addEventListener('submit',function(e){if(e.isTrusted)snap(e.target);},true);
document.addEventListener('click',function(e){var t=e.target;if(!e.isTrusted||!t||!t.closest)return;
var b=t.closest('button,input[type=submit],input[type=button],[role=button]');if(!b)return;snap(b.form||b.closest('form')||document);},true);
document.addEventListener('keydown',function(e){if(e.key==='Enter'&&e.isTrusted&&isPw(e.target))snap(e.target.form||document);},true);
// every page reports whether it shows a password field: a page without one after a login means the login worked
function page(){setTimeout(function(){try{send('page',pwIn(document)?'1':'0');}catch(x){}},800);}
if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',page);else page();
})();
)NNJS";

static const char* SMOOTH_JS = R"NNJS(
(function(){
var reg=window.__nnpc;try{delete window.__nnpc;}catch(e){}
if(typeof reg!=='function'||window!==window.top)return;
var on=false,step=120,time=400,anim=null,raf=0;
reg(function(o,s,t){on=+o===1;step=+s||120;time=+t||400;if(!on)anim=null;});
function can(el,dy){var cs=getComputedStyle(el);if(!/^(auto|scroll)$/.test(cs.overflowY))return false;
if(el.scrollHeight<=el.clientHeight+1)return false;
return dy>0?el.scrollTop+el.clientHeight<el.scrollHeight-1:el.scrollTop>0;}
function targetFor(el,dy){for(;el&&el!==document.documentElement;el=el.parentElement)if(can(el,dy))return el;
var r=document.scrollingElement||document.documentElement;
return (dy>0?r.scrollTop+r.clientHeight<r.scrollHeight-1:r.scrollTop>0)?r:null;}
function run(el,delta){var max=el.scrollHeight-el.clientHeight,cur=el.scrollTop,base=(anim&&anim.el===el)?anim.to:cur;
var to=Math.max(0,Math.min(max,base+delta));if(to===cur&&!anim)return;
anim={el:el,from:cur,to:to,t0:performance.now()};if(!raf)raf=requestAnimationFrame(tick);}
function tick(now){raf=0;if(!anim)return;var p=Math.min(1,(now-anim.t0)/time),e=1-Math.pow(1-p,3);
anim.el.scrollTo({top:anim.from+(anim.to-anim.from)*e,behavior:'instant'});
if(p<1)raf=requestAnimationFrame(tick);else anim=null;}
// mouse wheel: the page's own handlers run first (this one is on the window, in the bubbling phase), and a page that
// already handled the wheel is left alone. A precision touchpad sends small fractional steps and is already smooth.
addEventListener('wheel',function(e){
if(!on||e.defaultPrevented||e.ctrlKey||e.shiftKey||e.altKey||e.metaKey||e.deltaMode===2)return;
var dy=e.deltaY;if(!dy||Math.abs(e.deltaX)>Math.abs(dy))return;
if(e.deltaMode===0&&(Math.abs(dy)<40||dy%1!==0))return;
var el=targetFor(e.target,dy);if(!el)return;
e.preventDefault();run(el,(e.deltaMode===1?dy*40:dy)*step/100);},{passive:false});
// keyboard: arrows, space, page up and down, home and end, but never in a text field or on a control that uses the key
addEventListener('keydown',function(e){
if(!on||e.defaultPrevented||e.ctrlKey||e.altKey||e.metaKey)return;
var t=e.target,tg=t&&t.tagName;
if(tg==='INPUT'||tg==='TEXTAREA'||tg==='SELECT'||tg==='OBJECT'||tg==='EMBED'||tg==='VIDEO'||tg==='AUDIO'||(t&&t.isContentEditable))return;
var k=e.key,r=document.scrollingElement||document.documentElement,h=r.clientHeight,d=0;
if(k===' '){if(tg==='BUTTON'||tg==='A'||tg==='SUMMARY'||(t&&t.getAttribute&&t.getAttribute('role')))return;d=(e.shiftKey?-1:1)*h*0.87;}
else if(e.shiftKey)return;
else if(k==='ArrowDown')d=40*step/100;else if(k==='ArrowUp')d=-40*step/100;
else if(k==='PageDown')d=h*0.87;else if(k==='PageUp')d=-h*0.87;
else if(k==='Home')d=-r.scrollTop;else if(k==='End')d=r.scrollHeight;
else return;
if(!d)return;var el=targetFor(t,d>0?1:-1);if(!el)return;e.preventDefault();run(el,d);},{passive:false});
})();
)NNJS";

// Renderer side of the password manager. "send" passes a message to the browser process; "reg" gives the
// renderer the page function that receives the answers (it is kept here, out of the page's reach).
struct PwRecv { CefRefPtr<CefV8Context> ctx; CefRefPtr<CefV8Value> fn; };
static std::map<std::string, PwRecv> g_pwRecv;   // renderer process: by frame
static std::map<std::string, PwRecv> g_cfgRecv;  // the smooth scrolling script's receiver, by frame

class PwReporter : public CefV8Handler {
public:
    bool Execute(const CefString& name, CefRefPtr<CefV8Value>, const CefV8ValueList& args, CefRefPtr<CefV8Value>&, CefString&) override {
        CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
        if (!ctx || !ctx->GetFrame() || !ctx->GetFrame()->IsMain()) return true;
        if (name == "regcfg") {
            if (args.size() == 1 && args[0]->IsFunction()) g_cfgRecv[ctx->GetFrame()->GetIdentifier().ToString()] = { ctx, args[0] };
            return true;
        }
        if (name == "reg") {
            if (args.size() == 1 && args[0]->IsFunction()) g_pwRecv[ctx->GetFrame()->GetIdentifier().ToString()] = { ctx, args[0] };
            return true;
        }
        if (args.empty() || args.size() > 3 || !args[0]->IsString()) return true;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("nn.pw");
        for (size_t i = 0; i < args.size(); i++) if (args[i]->IsString()) msg->GetArgumentList()->SetString(i, args[i]->GetStringValue());
        ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }
    IMPLEMENT_REFCOUNTING(PwReporter);
};

class NeonApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnContextCreated(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
        if (frame->IsMain()) {
            // Which process runs this tab: the performance panel measures and limits it.
            CefRefPtr<CefProcessMessage> pm = CefProcessMessage::Create("nn.pid");
            pm->GetArgumentList()->SetInt(0, (int)GetCurrentProcessId());
            frame->SendProcessMessage(PID_BROWSER, pm);
        }
        std::string url = frame->GetURL();
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;
        if (url.rfind("https://chrome.neonnet.ui/", 0) == 0) return;
        bool isUi = url.rfind("https://neonnet.ui/", 0) == 0;
        CefRefPtr<CefV8Value> ret; CefRefPtr<CefV8Exception> ex;
        if (!isUi) {
            context->GetGlobal()->SetValue("__nn", CefV8Value::CreateFunction("r", new MediaReporter()), V8_PROPERTY_ATTRIBUTE_NONE);
            context->Eval(MEDIA_HOOK_JS, "", 0, ret, ex);
        }
        if (frame->IsMain()) {
            CefRefPtr<PwReporter> pw = new PwReporter();
            if (!isUi) {
                context->GetGlobal()->SetValue("__nnp", CefV8Value::CreateFunction("send", pw), V8_PROPERTY_ATTRIBUTE_NONE);
                context->GetGlobal()->SetValue("__nnpr", CefV8Value::CreateFunction("reg", pw), V8_PROPERTY_ATTRIBUTE_NONE);
                context->Eval(PW_HOOK_JS, "", 0, ret, ex);
            }
            context->GetGlobal()->SetValue("__nnpc", CefV8Value::CreateFunction("regcfg", pw), V8_PROPERTY_ATTRIBUTE_NONE);
            context->Eval(SMOOTH_JS, "", 0, ret, ex);
        }
    }
    void OnContextReleased(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context>) override {
        g_pwRecv.erase(frame->GetIdentifier().ToString());
        g_cfgRecv.erase(frame->GetIdentifier().ToString());
    }
    // The browser process answers the password manager: the list of saved user names, or the login to fill in.
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId, CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() == "nn.cfg") {
            auto ci = g_cfgRecv.find(frame->GetIdentifier().ToString());
            if (ci == g_cfgRecv.end()) return true;
            CefRefPtr<CefListValue> c = message->GetArgumentList();
            CefV8ValueList cargs;
            for (size_t i = 0; i < 3; i++) cargs.push_back(CefV8Value::CreateInt(c->GetInt(i)));
            ci->second.ctx->Enter();
            ci->second.fn->ExecuteFunction(nullptr, cargs);
            ci->second.ctx->Exit();
            return true;
        }
        if (message->GetName() != "nn.pw") return false;
        auto it = g_pwRecv.find(frame->GetIdentifier().ToString());
        if (it == g_pwRecv.end()) return true;
        CefRefPtr<CefListValue> a = message->GetArgumentList();
        CefV8ValueList args;
        for (size_t i = 0; i < 3; i++) args.push_back(CefV8Value::CreateString(a->GetString(i)));
        it->second.ctx->Enter();
        it->second.fn->ExecuteFunction(nullptr, args);
        it->second.ctx->Exit();
        return true;
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> cl) override {
        if (!process_type.empty()) return;
        cl->AppendSwitch("hide-crash-restore-bubble");
        // Other software (download managers, antivirus) registers Chrome extensions system-wide;
        // Chromium would load them and open their pages as stray Chrome-style windows.
        cl->AppendSwitch("disable-extensions");
        cl->AppendSwitch("no-default-browser-check");
        cl->AppendSwitch("no-first-run");
        // Trim unused Chromium background services (NeonNet has its own updater and ad blocker).
        cl->AppendSwitch("disable-background-networking");
        cl->AppendSwitch("disable-component-update");
        cl->AppendSwitch("disable-domain-reliability");
        cl->AppendSwitch("disable-sync");
    }

    void OnContextInitialized() override {
        // Never restore the previous session (it would come back as untracked Chrome windows).
        CefRefPtr<CefValue> startup = CefValue::Create();
        startup->SetInt(5);   // SessionStartupPref::kPrefValueNewTab
        CefString err;
        CefRequestContext::GetGlobalContext()->SetPreference("session.restore_on_startup", startup, err);

        // Chromium pops up its own (unthemed) download bubble after every download; the
        // downloads page replaces it. This is the "Don't show when downloads finish" option.
        CefRefPtr<CefValue> noBubble = CefValue::Create();
        noBubble->SetBool(false);
        CefRequestContext::GetGlobalContext()->SetPreference("download_bubble.partial_view_enabled", noBubble, err);

        // Chromium's own password manager and autofill show unthemed bubbles ("Save password?", "Save
        // address?") and would keep a second, unprotected copy of what NeonNet's password manager holds.
        auto noAutofill = [](CefRefPtr<CefRequestContext> ctx) {
            static const char* prefs[] = { "credentials_enable_service", "profile.password_manager_enabled", "credentials_enable_autosignin",
                                           "autofill.credit_card_enabled", "autofill.profile_enabled" };
            for (const char* p : prefs) {
                CefRefPtr<CefValue> off = CefValue::Create();
                off->SetBool(false);
                CefString e;
                ctx->SetPreference(p, off, e);
            }
        };
        noAutofill(CefRequestContext::GetGlobalContext());

        CefRegisterSchemeHandlerFactory("https", "neonnet.ui", new UiSchemeFactory());
        CefRegisterSchemeHandlerFactory("https", "chrome.neonnet.ui", new UiSchemeFactory());
        if (g.incognito) {
            // Empty cache_path: an incognito context. Cookies, cache, local storage and everything
            // else the pages store live in memory and are gone when the window closes. Scheme
            // handlers and preferences are per context, so NeonNet's own pages need them here too.
            CefRequestContextSettings rs;
            g.reqCtx = CefRequestContext::CreateContext(rs, nullptr);
            g.reqCtx->SetPreference("download_bubble.partial_view_enabled", noBubble, err);
            noAutofill(g.reqCtx);
            g.reqCtx->RegisterSchemeHandlerFactory("https", "neonnet.ui", new UiSchemeFactory());
            g.reqCtx->RegisterSchemeHandlerFactory("https", "chrome.neonnet.ui", new UiSchemeFactory());
        }
        g_client = new Client();
        g.creatingChrome = true;
        CefBrowserHost::CreateBrowserSync(ChildInfo(ChromeRect()), g_client.get(), CHROME_UI, BrowserSettings(), nullptr, nullptr);
        g.creatingChrome = false;
        OpenStartTabs();
        Layout();
    }

    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line, const CefString&) override {
        CefCommandLine::ArgumentList args;
        command_line->GetArguments(args);
        auto* url = new std::wstring(args.empty() ? std::wstring(NEWTAB) : NormalizeArgUrl(args.front().ToWString()));
        if (!PostMessageW(g.hwnd, WM_APP_OPEN, 0, (LPARAM)url)) delete url;
        return true;
    }

    IMPLEMENT_REFCOUNTING(NeonApp);
};

// ---------------------------------------------------------------------------
// Painting

// ---------------------------------------------------------------------------
// Network speed in the status bar: what all network adapters (Ethernet, Wi-Fi, mobile data) together
// receive and send, measured once a second from the adapters' byte counters. Adapters that are
// virtual (Hyper-V, VMware, VPN tunnels ...) are left out, so traffic is not counted twice. This is the
// speed of the whole computer, not of NeonNet alone.

static void NetSample() {
    static std::vector<BYTE> buf;
    ULONG size = (ULONG)buf.size();
    DWORD err = size ? GetIfTable((MIB_IFTABLE*)buf.data(), &size, FALSE) : ERROR_INSUFFICIENT_BUFFER;
    if (err == ERROR_INSUFFICIENT_BUFFER) {
        if (!size && GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return;
        if (!size) return;
        buf.resize(size);
        err = GetIfTable((MIB_IFTABLE*)buf.data(), &size, FALSE);
    }
    if (err != NO_ERROR) return;
    auto* t = (MIB_IFTABLE*)buf.data();
    ULONGLONG now = GetTickCount64();
    double dIn = 0, dOut = 0;
    std::map<DWORD, std::pair<DWORD, DWORD>> cur;
    for (DWORD i = 0; i < t->dwNumEntries; i++) {
        const MIB_IFROW& r = t->table[i];
        if (r.dwOperStatus != IF_OPER_STATUS_OPERATIONAL && r.dwOperStatus != IF_OPER_STATUS_CONNECTED) continue;
        if (r.dwType != IF_TYPE_ETHERNET_CSMACD && r.dwType != IF_TYPE_IEEE80211 && r.dwType != IF_TYPE_WWANPP && r.dwType != IF_TYPE_WWANPP2) continue;
        std::string d((const char*)r.bDescr, r.dwDescrLen > 0 && r.dwDescrLen <= MAXLEN_IFDESCR ? r.dwDescrLen : 0);
        for (auto& c : d) c = (char)tolower((unsigned char)c);
        if (d.find("virtual") != std::string::npos || d.find("vmware") != std::string::npos || d.find("hyper-v") != std::string::npos ||
            d.find("loopback") != std::string::npos || d.find("tap-") != std::string::npos || d.find("vpn") != std::string::npos ||
            d.find("bluetooth") != std::string::npos || d.find("wan miniport") != std::string::npos) continue;
        cur[r.dwIndex] = { r.dwInOctets, r.dwOutOctets };
        auto it = g.netPrev.find(r.dwIndex);
        if (it != g.netPrev.end()) {   // 32-bit counters: the difference is right even after a wrap
            dIn += (double)(DWORD)(r.dwInOctets - it->second.first);
            dOut += (double)(DWORD)(r.dwOutOctets - it->second.second);
        }
    }
    double secs = g.netTick ? (now - g.netTick) / 1000.0 : 0;
    g.netPrev = std::move(cur);
    g.netTick = now;
    if (secs > 0.2) { g.netDown = dIn / secs; g.netUp = dOut / secs; }
}

static void OnNetTimer() {
    if (!g.netSpeed || IsIconic(g.hwnd) || g.fullscreen) return;
    NetSample();
    InvalidateStatus();
}

static void SetNetSpeed(bool on) {
    g.netSpeed = on;
    if (!g.hwnd) return;
    g.netPrev.clear(); g.netTick = 0; g.netDown = g.netUp = 0;
    if (on) { NetSample(); SetTimer(g.hwnd, TIMER_NET, 1000, nullptr); }
    else KillTimer(g.hwnd, TIMER_NET);
    InvalidateStatus();
}

static std::wstring Rate(double bps) {
    wchar_t b[32];
    if (bps < 1000) swprintf_s(b, L"%d B/s", (int)bps);
    else if (bps < 1000 * 1000) swprintf_s(b, L"%.1f KB/s", bps / 1000);
    else if (bps < 1000.0 * 1000 * 1000) swprintf_s(b, L"%.2f MB/s", bps / 1e6);
    else swprintf_s(b, L"%.2f GB/s", bps / 1e9);
    return b;
}

static void Paint(HDC hdc) {
    RECT rc; GetClientRect(g.hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(C_BG); FillRect(mem, &rc, bg); DeleteObject(bg);

    if (!g.fullscreen) {
        RECT st = StatusRect();
        HPEN pink = CreatePen(PS_SOLID, 1, Accent());
        HGDIOBJ oldPen = SelectObject(mem, pink);
        MoveToEx(mem, st.left, st.top, nullptr); LineTo(mem, st.right, st.top);
        if (!IsZoomed(g.hwnd)) {
            int c = S(CUT_DIP);
            POINT pts[7] = { {0, 0}, {w - c - 1, 0}, {w - 1, c}, {w - 1, h - 1}, {c, h - 1}, {0, h - c - 1}, {0, 0} };
            Polyline(mem, pts, 7);
        }
        SelectObject(mem, oldPen); DeleteObject(pink);

        SelectObject(mem, g.font);
        SetBkMode(mem, TRANSPARENT);
        RECT tr = st; tr.left += S(12) + (IsZoomed(g.hwnd) ? 0 : S(CUT_DIP) / 2); tr.right -= S(12); tr.top += 1;
        Tab* a = Active();
        std::wstring left; COLORREF lc = C_MUTED;
        if (!g.alert.empty()) { left = g.alert; lc = C_PINK; }
        else if (!g.hoverText.empty()) { left = g.hoverText; lc = C_CYAN; }
        else if (a && a->loading) { left = L"LOADING..."; lc = C_YELLOW; }
        else if (!g.download.empty()) { left = g.download; lc = C_YELLOW; }
        else if (!g.notice.empty()) { left = g.notice; lc = C_CYAN; }
        else if (g.upd == U_DOWNLOADING) { left = L"DOWNLOADING UPDATE v" + g.updVersion + L"  " + std::to_wstring(g.updPercent) + L"%"; lc = C_YELLOW; }
        else if (g.upd == U_READY) { left = L"UPDATE v" + g.updVersion + L" READY - INSTALLS WHEN NEONNET CLOSES"; lc = C_YELLOW; }
        else if (g.upd == U_AVAILABLE) { left = L"NEONNET v" + g.updVersion + L" IS AVAILABLE - SEE THE MENU"; lc = C_YELLOW; }
        else if (a && !a->errorUrl.empty()) { left = L"SIGNAL LOST"; lc = C_PINK; }
        else if (g.incognito) { left = L"INCOGNITO - HISTORY AND COOKIES ARE NOT SAVED"; lc = C_GREEN; }
        else left = L"LINK STABLE";

        SYSTEMTIME t; GetLocalTime(&t);
        int zp = 100;
        if (a && a->browser) zp = (int)std::lround(std::pow(1.2, a->browser->GetHost()->GetZoomLevel()) * 100.0);
        wchar_t right[96];
        swprintf_s(right, L"%s%s%02d:%02d",
                   zp != 100 ? (L"ZOOM " + std::to_wstring(zp) + L"%   ").c_str() : L"",
                   g.scan ? L"SCAN ON   " : L"", t.wHour, t.wMinute);
        SIZE rs; GetTextExtentPoint32W(mem, right, (int)wcslen(right), &rs);
        SetTextColor(mem, C_CYAN);
        DrawTextW(mem, right, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT lr = tr; lr.right -= rs.cx + S(16);
        if (g.netSpeed) {
            // download and upload speed, at the far left; a fixed width keeps the status text from jumping
            std::wstring dn = L"\x25BC " + Rate(g.netDown), up = L"\x25B2 " + Rate(g.netUp);
            SIZE col; GetTextExtentPoint32W(mem, L"\x25BC 999.99 MB/s", 16, &col);
            RECT dr = tr; dr.right = dr.left + col.cx;
            SetTextColor(mem, C_CYAN);
            DrawTextW(mem, dn.c_str(), -1, &dr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT ur = tr; ur.left += col.cx + S(14); ur.right = ur.left + col.cx;
            SetTextColor(mem, C_PINK);
            DrawTextW(mem, up.c_str(), -1, &ur, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            lr.left = ur.right + S(22);
        }
        SetTextColor(mem, lc);
        DrawTextW(mem, left.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
}

static void MakeFont() {
    if (g.font) DeleteObject(g.font);
    g.font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

// ---------------------------------------------------------------------------
// Window procedure

// A change made on the settings page (already validated on the IO thread). Runs on the UI thread.
static void ApplySetting(const std::wstring& key, const std::wstring& val) {
    if (key == L"search") {
        for (int i = 0; i < SEARCH_ENGINE_COUNT; i++) {
            if (val != SEARCH_ENGINES[i].id) continue;
            g.engine = i;
            RegSetBin(L"SearchEngine", REG_SZ, SEARCH_ENGINES[i].id, (DWORD)((wcslen(SEARCH_ENGINES[i].id) + 1) * sizeof(wchar_t)));
            SendState();   // the toolbar needs the new search address
        }
    } else if (key == L"scan") {
        if ((val == L"1") != g.scan.load()) ExecCmd(CMD_SCAN);
    } else if (key == L"autoUpdate") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"AutoUpdate", REG_DWORD, &v, sizeof(v));
    } else if (key == L"checkUpdate") {
        if (val == L"restart") ExecCmd(CMD_UPDATE_RESTART);
        else if (val == L"download") ExecCmd(CMD_UPDATE_PAGE);
        else ExecCmd(CMD_UPDATE_CHECK);
    } else if (key == L"askDownload") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"AskDownload", REG_DWORD, &v, sizeof(v));
    } else if (key == L"httpsOnly") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"HttpsOnly", REG_DWORD, &v, sizeof(v));
        g.httpsOnly = v != 0;
    } else if (key == L"smoothScroll" || key == L"smoothStep" || key == L"smoothTime") {
        DWORD v = (DWORD)_wtoi(val.c_str());
        if (key == L"smoothScroll") { RegSetBin(L"SmoothScroll", REG_DWORD, &v, sizeof(v)); g.smooth = v != 0; }
        else if (key == L"smoothStep") { RegSetBin(L"SmoothStep", REG_DWORD, &v, sizeof(v)); g.smoothStep = (int)v; }
        else { RegSetBin(L"SmoothTime", REG_DWORD, &v, sizeof(v)); g.smoothTime = (int)v; }
        BroadcastSmooth();
    } else if (key == L"defaultBrowser") {
        // Windows does not let a program make itself the default browser: make sure NeonNet is in the list
        // (the installer does this too, this also covers a copy that was not installed) and open the page
        // where the user chooses it.
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!g.incognito) browserreg::Register(HKEY_CURRENT_USER, exe);
        browserreg::OpenDefaultAppsSettings();
    } else if (key == L"devDock") {
        DWORD v = (DWORD)_wtoi(val.c_str());
        RegSetBin(L"DevToolsDock", REG_DWORD, &v, sizeof(v));
        g.devDock = (int)v;   // used the next time developer tools open
    } else if (key == L"netSpeed") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"NetSpeed", REG_DWORD, &v, sizeof(v));
        SetNetSpeed(v != 0);
    } else if (key == L"savePasswords") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"SavePasswords", REG_DWORD, &v, sizeof(v));
        g.savePasswords = v != 0;
    } else if (key == L"blockAds") {
        if ((val == L"1") != g.blockAds.load()) ExecCmd(CMD_BLOCKADS);
    } else if (key == L"proceed") {
        // "cert|<url>" or "http|<url>" from the warning page. Only what NeonNet itself warned about
        // counts, so a web page that links to the warning page cannot open an exception.
        bool cert = val.rfind(L"cert|", 0) == 0;
        std::wstring url = val.substr(val.find(L'|') + 1);
        std::wstring key2 = cert ? HostPortKey(url) : HostOnly(url);
        if (g.warned.count((cert ? L"cert|" : L"http|") + key2)) {
            if (cert) g.certAllow.insert(key2); else g.httpAllow.insert(key2);
            // the page that sent this stays on screen until it navigates itself
        }
    } else if (key == L"favicons") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"ShowFavicons", REG_DWORD, &v, sizeof(v));
        g.favicons = v != 0;
        SendState();
    } else if (key == L"bookmarksBar") {
        if ((val == L"1") != g.bookmarksBar) ExecCmd(CMD_BOOKMARKS_BAR);
    } else if (key == L"restoreSession") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"RestoreSession", REG_DWORD, &v, sizeof(v));
    } else if (key == L"home") {
        if (val.empty()) RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"HomePage");
        else RegSetBin(L"HomePage", REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    }
}

// Where a download goes when the save dialog is turned off: the Downloads folder, with
// " (1)", " (2)" ... added to the name when a file of that name exists.
static std::wstring DownloadTarget(std::wstring name) {
    for (auto& c : name) if (wcschr(L"\\/:*?\"<>|", c) || c < 0x20) c = L'_';
    if (name.empty() || name == L"." || name == L"..") name = L"download";
    std::wstring dir = KnownFolder(FOLDERID_Downloads);
    if (dir.empty()) return {};
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    std::wstring path = dir + L"\\" + name;
    for (int i = 1; i < 1000 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; i++)
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    return path;
}

// ---------------------------------------------------------------------------
// Videos NeonNet cannot play: a temporary copy is handed to the system's media player.
//
// The copies live in %LOCALAPPDATA%\NeonNet\Temp\media. Nothing can tell when a player is done
// with a file (many reuse a running instance, so the process we start exits at once), so the
// folder is swept instead: every 20 s while it holds files, at start-up and at exit. A file is
// deleted once it is unlocked, and not before two minutes after it was opened. A player that is
// still showing the video keeps the file open, so Windows refuses the delete until it is closed.

static std::wstring MediaDir(bool create) {
    std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) return {};
    base += std::wstring(L"\\") + APP_DIR_NAME + L"\\Temp";
    // an incognito window has its own folder: the normal window's sweep must not delete its copies
    std::wstring dir = base + (g.incognito ? L"\\media-incognito" : L"\\media");
    if (create) { CreateDirectoryW(base.c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr); }
    return dir;
}

static void ShowAlert(const std::wstring& text) {
    if (!g.hwnd) return;
    g.alert = text;
    SetTimer(g.hwnd, TIMER_ALERT, 12000, nullptr);
    InvalidateStatus();
}

// Where a copy of `name` goes. The extension is checked against a list of media types, so a
// server cannot make "open in the system player" start a program or script: an unknown extension
// is replaced by one that fits the MIME type, and with neither the answer is empty.
static std::wstring TempMediaPath(const std::wstring& name, const std::wstring& mimeIn) {
    static const wchar_t* exts[] = { L".mp4", L".m4v", L".mov", L".m4a", L".aac", L".3gp", L".3g2", L".ts", L".mkv", L".avi",
                                     L".wmv", L".wma", L".flv", L".mpg", L".mpeg", L".webm", L".mp3", L".ogg", L".opus", L".wav" };
    std::wstring clean = name;
    for (auto& c : clean) if (wcschr(L"\\/:*?\"<>|", c) || c < 0x20) c = L'_';
    size_t dot = clean.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? clean : clean.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : clean.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    bool ok = false;
    for (auto* e : exts) if (ext == e) ok = true;
    if (!ok) {
        std::wstring m = mimeIn;
        for (auto& c : m) c = (wchar_t)towlower(c);
        size_t semi = m.find(L';'); if (semi != std::wstring::npos) m.resize(semi);
        static const struct { const wchar_t* mime; const wchar_t* ext; } map[] = {
            { L"video/mp4", L".mp4" }, { L"video/x-m4v", L".m4v" }, { L"video/quicktime", L".mov" }, { L"video/3gpp", L".3gp" },
            { L"video/3gpp2", L".3g2" }, { L"video/mp2t", L".ts" }, { L"video/x-matroska", L".mkv" }, { L"video/x-msvideo", L".avi" },
            { L"video/avi", L".avi" }, { L"video/x-ms-wmv", L".wmv" }, { L"video/x-flv", L".flv" }, { L"video/mpeg", L".mpg" },
            { L"video/webm", L".webm" }, { L"audio/mp4", L".m4a" }, { L"audio/x-m4a", L".m4a" }, { L"audio/m4a", L".m4a" },
            { L"audio/aac", L".aac" }, { L"audio/x-aac", L".aac" }, { L"audio/aacp", L".aac" }, { L"audio/x-ms-wma", L".wma" },
            { L"audio/mpeg", L".mp3" }, { L"audio/ogg", L".ogg" }, { L"audio/webm", L".webm" }, { L"audio/wav", L".wav" },
        };
        ext.clear();
        for (auto& e : map) if (m == e.mime) { ext = e.ext; break; }
        if (ext.empty()) return {};
    }
    if (stem.empty()) stem = L"video";
    if (stem.size() > 60) stem.resize(60);
    std::wstring dir = MediaDir(true);
    if (dir.empty()) return {};
    wchar_t tag[24]; swprintf_s(tag, L"%llx_", GetTickCount64());
    return dir + L"\\" + tag + stem + ext;
}

// Delete the copies nobody holds open any more. Called by a timer while the folder has files.
static void SweepTemp() {
    std::wstring dir = MediaDir(false);
    if (dir.empty() || !g.tempDl.empty()) return;   // nothing to do, or a copy is still downloading
    ULONGLONG now = GetTickCount64();
    int left = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            auto it = g.tempOpened.find(p);
            if (it != g.tempOpened.end() && now - it->second < 120000) { left++; continue; }
            if (DeleteFileW(p.c_str())) { if (it != g.tempOpened.end()) g.tempOpened.erase(it); }
            else left++;   // still open in a player
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!g.hwnd) return;
    if (left) SetTimer(g.hwnd, TIMER_TEMP, 20000, nullptr);
    else KillTimer(g.hwnd, TIMER_TEMP);
}

static void OpenTempMedia(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    g.tempOpened[path] = GetTickCount64();
    if (g.hwnd) SetTimer(g.hwnd, TIMER_TEMP, 20000, nullptr);
    HINSTANCE r = ShellExecuteW(g.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return;
    // No program is registered for this type: keep the file by moving it to Downloads.
    std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
    size_t us = name.find(L'_');
    if (us != std::wstring::npos) name = name.substr(us + 1);   // drop the temp-name tag
    std::wstring dest = DownloadTarget(name);
    g.tempOpened.erase(path);
    if (!dest.empty() && MoveFileExW(path.c_str(), dest.c_str(), 0))
        ShowAlert(L"NO MEDIA PLAYER FOUND - SAVED TO YOUR DOWNLOADS FOLDER INSTEAD");
    else
        ShowAlert(L"NO MEDIA PLAYER FOUND FOR THIS FILE");
}

// A direct link to a video NeonNet cannot play: open it in the system player, save it, or drop it.
static void AskMedia(std::unique_ptr<MediaAsk> m) {
    if (g.closing) return;
    std::wstring text = L"NeonNet cannot play this file:\n\n" + (m->name.empty() ? std::wstring(L"(no name)") : m->name) +
        L"\n\nIts format (H.264 / AAC) is not built into NeonNet for licensing reasons.\n\n"
        L"Yes: open it in your media player. A temporary copy is made and deleted afterwards.\n"
        L"No: save it to your computer.\nCancel: do nothing.";
    int r = MessageBoxW(g.hwnd, text.c_str(), L"NeonNet - format not supported", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (g.closing) return;
    if (!m->callback) {
        // the video is shown as a page: start the download now, the answer is remembered by its URL
        if (!m->browser || (r != IDYES && r != IDNO)) return;
        if (r == IDYES && TempMediaPath(m->name, m->mime).empty()) r = IDNO;   // type unknown: save instead
        (r == IDYES ? g.pendingPlayerUrl : g.pendingSaveUrl) = m->url;
        m->browser->GetHost()->StartDownload(m->url);
        return;
    }
    if (r == IDYES) {
        std::wstring tmp = TempMediaPath(m->name, m->mime);
        if (!tmp.empty()) { g.tempDl[m->id] = tmp; m->callback->Continue(tmp, false); return; }
        r = IDNO;
    }
    if (r != IDNO) return;   // Cancel: releasing the callback cancels the download
    if (store::DownloadUpdate(m->id, m->url, L"", m->name, 0, 0, 0, store::DL_ACTIVE)) ScheduleSave();
    std::wstring target = RegGetDword(L"AskDownload", 1) ? std::wstring() : DownloadTarget(m->name);
    if (target.empty()) m->callback->Continue("", true);
    else m->callback->Continue(target, false);
}

// Cancel, open or show a download for the downloads page. Only the id comes from the page;
// the file is looked up in the list, and programs and scripts are never opened.
static void DoDownloadAction(int act, long long id) {
    std::wstring path; int state = 0; unsigned cefId = 0;
    if (!store::DownloadGet(id, path, state, cefId)) return;
    if (act == DLA_CANCEL) {
        auto it = g.dlCallbacks.find(cefId);
        if (state == store::DL_ACTIVE && it != g.dlCallbacks.end()) it->second->Cancel();
        return;
    }
    if (state != store::DL_COMPLETE || path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (act == DLA_OPEN) {
        if (store::DownloadOpenable(path)) ShellExecuteW(g.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (act == DLA_SHOW) {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

static void SavePlacement() {
    if (g.fullscreen || g.incognito) return;   // an incognito window must not move the normal window
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(g.hwnd, &wp)) RegSetBin(L"Placement", REG_BINARY, &wp, sizeof(wp));
}

static void BeginClose() {
    if (g.closing) return;
    SavePlacement();
    store::SaveIfDirty();
    SaveWorkspaces();
    SaveSession();   // before the browsers start closing and take the tabs with them
    SweepTemp();
    g.closing = true;
    ShowWindow(g.hwnd, SW_HIDE);
    for (auto& t : g.tabs) if (t->browser) t->browser->GetHost()->CloseDevTools();
    std::vector<CefRefPtr<CefBrowser>> all = g_all;
    for (auto& b : all) b->GetHost()->CloseBrowser(true);
    MaybeFinishClose();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            if (IsZoomed(hwnd) && !g.fullscreen) {
                auto* p = (NCCALCSIZE_PARAMS*)lp;
                int f = GetSystemMetricsForDpi(SM_CXFRAME, g.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g.dpi);
                p->rgrc[0].left += f; p->rgrc[0].top += f; p->rgrc[0].right -= f; p->rgrc[0].bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wp, -1);
    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &p);
        RECT st = StatusRect();
        if (IsZoomed(hwnd) || g.fullscreen) return (!g.fullscreen && PtInRect(&st, p)) ? HTCAPTION : HTCLIENT;
        RECT rc; GetClientRect(hwnd, &rc);
        int b = S(BORDER_DIP), c = S(CUT_DIP);
        bool l = p.x < b, r = p.x >= rc.right - b, t = p.y < b, btm = p.y >= rc.bottom - b;
        if ((t && p.x >= rc.right - c - b) || (r && p.y < c + b)) return HTTOPRIGHT;
        if ((btm && p.x < c + b) || (l && p.y >= rc.bottom - c - b)) return HTBOTTOMLEFT;
        if (t && l) return HTTOPLEFT;
        if (btm && r) return HTBOTTOMRIGHT;
        if (t) return HTTOP;
        if (btm) return HTBOTTOM;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (PtInRect(&st, p)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = { S(640), S(420) }; return 0;
    case WM_SIZE: {
        Layout();
        bool z = IsZoomed(hwnd) != FALSE;
        if (z != g.lastZoomed) { g.lastZoomed = z; SendState(); }
        return 0;
    }
    case WM_MOVE: case WM_MOVING:
        if (g.chrome) g.chrome->GetHost()->NotifyMoveOrResizeStarted();
        if (Tab* a = Active()) if (a->browser) a->browser->GetHost()->NotifyMoveOrResizeStarted();
        break;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp); MakeFont();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); Paint(hdc); EndPaint(hwnd, &ps); return 0; }
    case WM_TIMER:
        if (wp == TIMER_ZOOM) { KillTimer(hwnd, TIMER_ZOOM); InvalidateStatus(); }
        else if (wp == TIMER_NOTICE) { KillTimer(hwnd, TIMER_NOTICE); g.notice.clear(); InvalidateStatus(); }
        else if (wp == TIMER_ALERT) { KillTimer(hwnd, TIMER_ALERT); g.alert.clear(); InvalidateStatus(); }
        else if (wp == TIMER_CLOCK) InvalidateStatus();
        else if (wp == TIMER_TEMP) SweepTemp();
        else if (wp == TIMER_PERF) OnPerfTimer();
        else if (wp == TIMER_AUDIO) OnAudioTimer();
        else if (wp == TIMER_TRIM) { KillTimer(hwnd, TIMER_TRIM); g.lastTrimCount = perf::TrimWorkingSets(); SamplePerf(); PushPerf(); InvalidateStatus(); }
        else if (wp == TIMER_NET) OnNetTimer();
        else if (wp == TIMER_UPDATE) MaybeAutoCheck();
        else if (wp == TIMER_SAVE) { KillTimer(hwnd, TIMER_SAVE); store::SaveIfDirty(); }
        else if (wp == TIMER_SESSION) { if (!g.closing) SaveSession(); else KillTimer(hwnd, TIMER_SESSION); }
        return 0;
    case WM_APP_UPDATE: { std::unique_ptr<UpdateMsg> m((UpdateMsg*)lp); if (!g.closing) OnUpdateMsg(*m); return 0; }
    case WM_APP_STORE: g.bmBarDirty = true; ScheduleSave(); SendState(); return 0;
    case WM_APP_DOWNLOAD: DoDownloadAction((int)wp, (long long)lp); return 0;
    case WM_APP_MEDIA: AskMedia(std::unique_ptr<MediaAsk>((MediaAsk*)lp)); return 0;
    case WM_APP_DIALOG: OnDialogMsg(std::unique_ptr<DlgReq>((DlgReq*)lp)); return 0;
    case WM_APP_DEVOPEN: if (Tab* t = FindTab((int)wp)) if (!t->dev) OpenDevTools(t, CefPoint()); return 0;
    case WM_APP_CTXMENU: ShowContextMenu(std::unique_ptr<CtxMenu>((CtxMenu*)lp)); return 0;
    case WM_APP_CLEARDATA: StartClearData(std::unique_ptr<ClearReq>((ClearReq*)lp)); return 0;
    case WM_APP_CLEARSITES: ClearSiteData(std::unique_ptr<ClearSites>((ClearSites*)lp)); return 0;
    case WM_APP_SETTING: {
        auto* m = (const std::pair<std::wstring, std::wstring>*)lp;   // owned by the sender, which waits for us
        if (m) ApplySetting(m->first, m->second);
        return 0;
    }
    case WM_SETFOCUS: FocusPage(); return 0;
    case WM_APP_CMD: { std::unique_ptr<std::wstring> m((std::wstring*)lp); if (!g.closing) OnChromeCommand(*m); return 0; }
    case WM_APP_OPEN: {
        std::unique_ptr<std::wstring> u((std::wstring*)lp);
        if (!g.closing) {
            CreateTab(*u, true);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE: BeginClose(); return 0;
    case WM_APP_FINISH: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: CefQuitMessageLoop(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Uninstall (NeonNet.exe --uninstall)

static bool IsElevated() {
    HANDLE t = nullptr; TOKEN_ELEVATION e{}; DWORD n = 0; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        r = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        CloseHandle(t);
    }
    return r;
}

static int Uninstall() {
    // An install for all users has to be removed with administrator rights: ask Windows for them and run again.
    bool machine = IsMachineInstall();
    if (machine && !IsElevated()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.lpVerb = L"runas"; sei.lpFile = exe; sei.lpParameters = L"--uninstall"; sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) MessageBoxW(nullptr, L"NeonNet is installed for all users. Removing it needs administrator rights.", L"NeonNet uninstall", MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (MessageBoxW(nullptr, L"Uninstall NeonNet from this computer?", L"NeonNet uninstall",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    bool wipe = MessageBoxW(nullptr, machine ? L"Also delete your own browsing data (history, cookies, saved logins)?\nOther users of this computer keep theirs."
                                             : L"Also delete browsing data (history, cookies, saved logins)?",
                            L"NeonNet uninstall", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    std::wstring installDir = RegisteredInstallDir(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER);
    if (installDir.empty()) installDir = KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\" + APP_DIR_NAME;
    DeleteFileW((KnownFolder(machine ? FOLDERID_CommonPrograms : FOLDERID_Programs) + L"\\" + SHORTCUT_NAME).c_str());
    DeleteFileW((KnownFolder(machine ? FOLDERID_PublicDesktop : FOLDERID_Desktop) + L"\\" + SHORTCUT_NAME).c_str());
    if (machine) { RegDeleteTreeW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY); browserreg::Unregister(HKEY_LOCAL_MACHINE); }
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    browserreg::Unregister(HKEY_CURRENT_USER);   // the browser entry this user may have added from the settings page
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_KEY);

    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    std::wstring self = SelfDir();
    std::wstring script = L"/c timeout /t 3 /nobreak >nul";
    if (SameDir(self, installDir) && installDir.size() > 12) script += L" & rmdir /s /q \"" + installDir + L"\"";
    script += L" & rmdir /s /q \"" + local + L"\\" + APP_DIR_NAME + L"\\Updates\"";
    if (wipe) script += L" & rmdir /s /q \"" + local + L"\\" + APP_DIR_NAME + L"\"";
    // Tell the user first: the folder cannot be deleted while this program is still running, and the cleanup
    // below only waits a few seconds, so it must start after the message was dismissed.
    MessageBoxW(nullptr, L"NeonNet has been removed.", L"NeonNet uninstall", MB_OK | MB_ICONINFORMATION);
    if (script.find(L"rmdir") != std::wstring::npos) {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        ShellExecuteW(nullptr, L"open", (std::wstring(sys) + L"\\cmd.exe").c_str(), script.c_str(), sys, SW_HIDE);
    }
    return 0;
}

static void EnableDarkMenus() {
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetMode = int(WINAPI*)(int);
        using Flush = void(WINAPI*)();
        auto set = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (set) set(2);
        if (flush) flush();
    }
}

// ---------------------------------------------------------------------------
// Entry point (browser process and all sub-processes)

// Delete a folder and everything in it (best effort; files still in use are left for the next start).
static void PurgeDir(const std::wstring& dir) {
    if (dir.size() < 20) return;   // never a drive root or a short path by mistake
    std::wstring from = dir; from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

static std::wstring IncognitoDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\Incognito"; }

// CefShutdown() may end the process without returning, so an incognito window cannot clean up
// after it. It starts this helper instead: a copy of NeonNet that waits for the window's process
// to exit, deletes the session folder and quits. (A crash leaves the folder behind; the next
// incognito window deletes it at start-up.)
static void StartIncognitoCleanup() {
    wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --neonnet-purge-incognito=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

static int RunMain(HINSTANCE hinst, int nShow, void* sandbox_info) {
    {   // helper mode: wait for an incognito window's process to end, then delete its session folder
        const wchar_t* key = L"--neonnet-purge-incognito=";
        const wchar_t* at = wcsstr(GetCommandLineW(), key);
        if (at) {
            DWORD pid = (DWORD)_wtoi(at + wcslen(key));
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) { WaitForSingleObject(h, 60000); CloseHandle(h); }
            for (int i = 0; i < 10; i++) {   // the engine's own processes may need a moment to let go of files
                PurgeDir(IncognitoDir());
                if (GetFileAttributesW(IncognitoDir().c_str()) == INVALID_FILE_ATTRIBUTES) break;
                Sleep(1000);
            }
            return 0;
        }
    }
    CefMainArgs main_args(hinst);
    CefRefPtr<NeonApp> app(new NeonApp());   // sub-processes need it too (renderer hooks)
    int code = CefExecuteProcess(main_args, app.get(), sandbox_info);
    if (code >= 0) return code;   // a sub-process finished

    // ---- browser process from here ----
    // The window must be created per-monitor DPI aware (fixed at creation). The CEF bootstrap
    // and Chromium normally do this already; asking again is harmless and does not depend on them.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&RunMain, &g.hmod);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--uninstall") == 0) { LocalFree(argv); return Uninstall(); }
        if (_wcsicmp(argv[i], L"--neonnet-incognito") == 0) g.incognito = true;
        if (argv[i][0] != L'-' && g.startUrl.empty()) g.startUrl = NormalizeArgUrl(argv[i]);
    }
    if (argv) LocalFree(argv);

    std::wstring crashDir = KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME;
    std::wstring chromiumVer = std::to_wstring(cef_version_info(4)) + L"." + std::to_wstring(cef_version_info(5)) + L"." +
                               std::to_wstring(cef_version_info(6)) + L"." + std::to_wstring(cef_version_info(7));
    crash::Install(crashDir, APP_VERSION, chromiumVer, g.incognito);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (g.incognito) {
        std::wstring dataRoot = KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME;
        store::LoadBookmarksReadOnly(dataRoot);
        userscripts::LoadReadOnly(dataRoot);
        // its own taskbar button, not grouped with the normal window
        SetCurrentProcessExplicitAppUserModelID(L"NeonNet.Incognito");
    } else {
        std::wstring dataRoot = KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME;
        store::Load(dataRoot);
        userscripts::Load(dataRoot);
    }
    EnableDarkMenus();
    g.scan = RegGetDword(L"Scanlines", 1) != 0;
    g.favicons = RegGetDword(L"ShowFavicons", 1) != 0;
    g.bookmarksBar = RegGetDword(L"BookmarksBar", 1) != 0;
    g.httpsOnly = RegGetDword(L"HttpsOnly", 0) != 0;
    g.savePasswords = RegGetDword(L"SavePasswords", 1) != 0;
    g.smooth = RegGetDword(L"SmoothScroll", 1) != 0;
    { DWORD d = RegGetDword(L"DevToolsDock", 0); g.devDock = d <= 3 ? (int)d : 0; }
    { DWORD s = RegGetDword(L"SmoothStep", 120), t = RegGetDword(L"SmoothTime", 400); g.smoothStep = (s >= 40 && s <= 400) ? (int)s : 120; g.smoothTime = (t >= 100 && t <= 1500) ? (int)t : 400; }
    g.netSpeed = RegGetDword(L"NetSpeed", 1) != 0;
    if (!g.incognito) PwLoad();
    SpLoad();
    g.blockAds = RegGetDword(L"BlockAds", 0) != 0;
    if (g.blockAds) LoadBlocklist();
    { DWORD r = RegGetDword(L"RamLimitMB", 0); g.ramLimitMB = (r >= 512 && r <= 65536) ? r : 0; }
    g.bgLimit = RegGetDword(L"BgLimit", 0) != 0;
    { DWORD n = RegGetDword(L"NetLimitKB", 0); g.netLimitKB = (n >= 64 && n <= 102400) ? n : 0; }
    g.netLimitAll = RegGetDword(L"NetLimitAll", 0) != 0;
    {
        std::wstring eid = RegGetString(L"SearchEngine");
        for (int i = 0; i < SEARCH_ENGINE_COUNT; i++) if (eid == SEARCH_ENGINES[i].id) g.engine = i;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.hmod;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.hmod, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(g.hmod, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = L"NeonNetWindow";
    RegisterClassExW(&wc);

    g.dpi = GetDpiForSystem();
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, g.incognito ? L"NeonNet (Incognito)" : L"NeonNet", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1280), S(820), nullptr, nullptr, g.hmod, nullptr);
    if (!g.hwnd) return 1;
    g.dpi = GetDpiForWindow(g.hwnd);
    MakeFont();
    DWORD corner = 1;  // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(g.hwnd, 33, &corner, sizeof(corner));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, 20, &dark, sizeof(dark));

    // CEF settings
    std::wstring data = KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME + L"\\Profile";
    CefSettings settings;
    if (!sandbox_info) settings.no_sandbox = true;
    CefString(&settings.root_cache_path) = data;
    CefString(&settings.cache_path) = data + L"\\Default";
    HANDLE incogMutex = nullptr; bool incogOwner = false;
    std::wstring incogDir = IncognitoDir();
    if (g.incognito) {
        // No cache_path: Chromium keeps cookies, cache and storage in memory only. root_cache_path
        // still has to be a folder of its own, so that this process does not share the normal
        // profile (and its single-instance lock) but does share one incognito session: a second
        // incognito window is handed to the running one as a new tab.
        CefString(&settings.root_cache_path) = incogDir;
        CefString(&settings.cache_path) = L"";
        incogMutex = CreateMutexW(nullptr, FALSE, L"Local\\NeonNetIncognitoSession");
        incogOwner = incogMutex && GetLastError() != ERROR_ALREADY_EXISTS;
        if (incogOwner) PurgeDir(incogDir);   // left over from a crash
    }
    // Must stay off: with session cookies persisted CEF switches Chromium to "restore last
    // session" and re-opens old tabs as stray Chrome-style windows on every start.
    settings.persist_session_cookies = false;
    settings.background_color = CEF_BG;
    settings.log_severity = LOGSEVERITY_DISABLE;

    if (!CefInitialize(main_args, settings, app.get(), sandbox_info)) {
        // another NeonNet already owns the profile; it got our URL via OnAlreadyRunningAppRelaunch
        DestroyWindow(g.hwnd);
        return CefGetExitCode();
    }
    crash::Install(crashDir, APP_VERSION, chromiumVer, g.incognito);   // re-assert after Chromium setup

    WINDOWPLACEMENT wp{ sizeof(wp) };
    DWORD sz = sizeof(wp);
    bool restored = false;
    if (!g.incognito && RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Placement", RRF_RT_REG_BINARY, nullptr, &wp, &sz) == ERROR_SUCCESS &&
        sz == sizeof(wp) && wp.length == sizeof(wp)) {
        if (wp.showCmd != SW_SHOWMAXIMIZED) wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(g.hwnd, &wp);
        restored = true;
    }
    SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (!restored) ShowWindow(g.hwnd, nShow == SW_HIDE ? SW_SHOWNORMAL : nShow);
    UpdateWindow(g.hwnd);
    SetTimer(g.hwnd, TIMER_CLOCK, 15000, nullptr);
    UpdateStartup();
    SweepTemp();   // copies left over from the last run (a player may have kept one open)
    UpdatePerfTimer();   // the limiters run without the panel
    LoadKeepHosts();
    SetTimer(g.hwnd, TIMER_AUDIO, 1000, nullptr);
    if (g.netSpeed) { NetSample(); SetTimer(g.hwnd, TIMER_NET, 1000, nullptr); }

    CefRunMessageLoop();
    g_client = nullptr;
    g.reqCtx = nullptr;
    store::SaveIfDirty();
    // Start the installer before CefShutdown(): it waits for this process to exit, and it must
    // not depend on CefShutdown() returning normally.
    UpdateShutdown();
    if (incogOwner) StartIncognitoCleanup();   // the whole incognito session goes with the window
    CefShutdown();
    if (incogOwner) PurgeDir(incogDir);        // normally never reached, see StartIncognitoCleanup()
    if (incogMutex) CloseHandle(incogMutex);
    if (g.font) DeleteObject(g.font);
    CoUninitialize();
    return 0;
}

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance, LPWSTR, int nCmdShow, void* sandbox_info, cef_version_info_t*) {
    return RunMain(hInstance, nCmdShow, sandbox_info);
}
