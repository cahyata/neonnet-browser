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
#include <string>
#include <vector>
#include <algorithm>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_scheme.h"
#include "resource.h"
#include "updater.h"
#include "store.h"
#include <windowsx.h>   // after CEF headers: its macros (GetFirstChild...) clash with cef_dom.h

// ---------------------------------------------------------------------------
// Constants

static const int BORDER_DIP = 5;
static const int CHROME_DIP = 84;
static const int STATUS_DIP = 24;
static const int CUT_DIP = 16;
static const int FIND_DIP = 34;   // find bar row, added under the chrome UI while open

static const COLORREF C_BG = RGB(0x12, 0x06, 0x2c);
static const COLORREF C_PINK = RGB(0xff, 0x2e, 0x97);
static const COLORREF C_CYAN = RGB(0x00, 0xe5, 0xff);
static const COLORREF C_YELLOW = RGB(0xff, 0xd6, 0x00);
static const COLORREF C_MUTED = RGB(0x8a, 0x6f, 0xc0);
static const cef_color_t CEF_BG = 0xFF12062C;

static const wchar_t* UI_HOST = L"https://neonnet.ui/";
static const wchar_t* NEWTAB = L"https://neonnet.ui/newtab.html";
// The toolbar lives on its own host: Chromium keeps zoom per host, so on neonnet.ui it would
// shrink or grow whenever the user zooms the new tab, history or downloads page.
static const wchar_t* CHROME_UI_HOST = L"https://chrome.neonnet.ui/";
static const wchar_t* CHROME_UI = L"https://chrome.neonnet.ui/chrome.html";
static const wchar_t* HISTORY_PAGE = L"https://neonnet.ui/history.html";
static const wchar_t* BOOKMARKS_PAGE = L"https://neonnet.ui/bookmarks.html";
static const wchar_t* DOWNLOADS_PAGE = L"https://neonnet.ui/downloads.html";
static const wchar_t* SETTINGS_PAGE = L"https://neonnet.ui/settings.html";
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

static const UINT_PTR TIMER_CLOCK = 1, TIMER_ZOOM = 2, TIMER_UPDATE = 3, TIMER_NOTICE = 4, TIMER_SAVE = 5;
static const ULONGLONG UPDATE_INTERVAL_MS = 24ull * 60 * 60 * 1000;   // check at most once a day

enum Cmd {
    CMD_NEWTAB = 1, CMD_CLOSETAB, CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SCAN,
    CMD_FULLSCREEN, CMD_DEVTOOLS, CMD_ABOUT, CMD_RELOAD, CMD_STOP, CMD_BACK, CMD_FWD,
    CMD_FOCUSURL, CMD_NEXTTAB, CMD_PREVTAB, CMD_HOME, CMD_PRINT,
    CMD_UPDATE_CHECK, CMD_UPDATE_AUTO, CMD_UPDATE_RESTART, CMD_UPDATE_PAGE,
    CMD_FIND, CMD_FINDNEXT, CMD_FINDPREV, CMD_REOPEN, CMD_BOOKMARK, CMD_BOOKMARKS, CMD_HISTORY,
    CMD_DOWNLOADS, CMD_SETTINGS
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
enum { MENU_OPEN_LINK_TAB = MENU_ID_USER_FIRST, MENU_INSPECT };

struct Tab {
    int id = 0;
    CefRefPtr<CefBrowser> browser;
    std::wstring title = L"NEW TAB";
    std::wstring url;
    std::wstring errorUrl;
    bool loading = true;
    bool canBack = false, canFwd = false;
    bool errorPending = false;
    long long histId = 0;   // history entry of the page shown
};

struct ClosedTab { std::wstring url; int index; };

struct App {
    HMODULE hmod = nullptr;   // NeonNet.dll (resources live here)
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    CefRefPtr<CefBrowser> chrome;
    std::atomic<int> chromeId{ 0 };
    std::vector<std::unique_ptr<Tab>> tabs;
    std::deque<int> pendingPopups;  // tab ids waiting for their popup browser
    int creatingTab = 0;            // tab id while CreateBrowserSync runs
    bool creatingChrome = false;
    int active = 0, nextId = 1;
    std::atomic<bool> scan{ true };   // atomic: the IO thread reads it for /api/settings
    int dropNeed = 0;                 // address bar suggestions open: toolbar height needed, CSS px (0 = closed)
    bool fsUser = false, fsElement = false, fullscreen = false;
    WINDOWPLACEMENT fsPrev{ sizeof(WINDOWPLACEMENT) };
    std::wstring hoverText, download;
    std::wstring startUrl;
    bool closing = false;
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
    // running downloads (UI thread only): CEF id -> handle used to cancel them
    std::map<unsigned, CefRefPtr<CefDownloadItemCallback>> dlCallbacks;
    // recently closed tabs, newest last
    std::vector<ClosedTab> closed;
};
static App g;

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

// History and bookmarks are written to disk shortly after they change.
static void ScheduleSave() { if (g.hwnd) SetTimer(g.hwnd, TIMER_SAVE, 2000, nullptr); }

static Tab* FindTab(int id) { for (auto& t : g.tabs) if (t->id == id) return t.get(); return nullptr; }
static Tab* TabOf(CefRefPtr<CefBrowser> b) {
    if (!b) return nullptr;
    int bid = b->GetIdentifier();
    for (auto& t : g.tabs) if (t->browser && t->browser->GetIdentifier() == bid) return t.get();
    return nullptr;
}
static bool IsChrome(CefRefPtr<CefBrowser> b) { return b && b->GetIdentifier() == g.chromeId.load(); }
static Tab* Active() { return FindTab(g.active); }
static int IndexOf(int id) { for (size_t i = 0; i < g.tabs.size(); i++) if (g.tabs[i]->id == id) return (int)i; return -1; }

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
    return { b, b, rc.right - b, b + S(CHROME_DIP + (g.findOpen ? FIND_DIP : 0)) };
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
    return { c.left, c.bottom, c.right, s.top };
}

static void UpdateRegion() {
    if (IsZoomed(g.hwnd) || g.fullscreen || IsIconic(g.hwnd)) { SetWindowRgn(g.hwnd, nullptr, TRUE); return; }
    RECT wr; GetWindowRect(g.hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top, c = S(CUT_DIP);
    POINT pts[6] = { {0, 0}, {w - c, 0}, {w, c}, {w, h}, {c, h}, {0, h - c} };
    SetWindowRgn(g.hwnd, CreatePolygonRgn(pts, 6, WINDING), TRUE);
}

static void Place(CefRefPtr<CefBrowser> b, const RECT& r, bool visible, bool raise = false) {
    if (!b) return;
    HWND w = b->GetHost()->GetWindowHandle();
    if (!w) return;
    bool wasVisible = IsWindowVisible(w) != FALSE;
    if (visible) {
        SetWindowPos(w, raise ? HWND_TOP : nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     (raise ? 0 : SWP_NOZORDER) | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!wasVisible) b->GetHost()->WasHidden(false);
    } else if (wasVisible) {
        ShowWindow(w, SW_HIDE);
        b->GetHost()->WasHidden(true);
    }
}

static void Layout() {
    if (!g.hwnd) return;
    if (g.chrome) Place(g.chrome, ChromeHostRect(), !g.fullscreen, g.dropNeed > 0);
    RECT content = ContentRect();
    for (auto& t : g.tabs) Place(t->browser, content, t->id == g.active);
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

static void SendState() {
    Tab* a = Active();
    if (a) SetWindowTextW(g.hwnd, ((a->title.empty() ? std::wstring(L"New tab") : a->title) + L" - NeonNet").c_str());
    if (!g.chrome) return;
    std::wstring j = L"{\"type\":\"state\",\"tabs\":[";
    for (size_t i = 0; i < g.tabs.size(); i++) {
        Tab* t = g.tabs[i].get();
        if (i) j += L",";
        j += L"{\"id\":" + std::to_wstring(t->id) + L",\"title\":" + JsonStr(t->title) +
             L",\"loading\":" + (t->loading ? L"true" : L"false") + L"}";
    }
    j += L"],\"active\":" + std::to_wstring(g.active);
    j += L",\"url\":" + JsonStr(a ? DisplayUrl(a) : L"");
    j += std::wstring(L",\"canBack\":") + (a && a->canBack ? L"true" : L"false");
    j += std::wstring(L",\"canFwd\":") + (a && a->canFwd && a->errorUrl.empty() ? L"true" : L"false");
    j += std::wstring(L",\"loading\":") + (a && a->loading ? L"true" : L"false");
    j += std::wstring(L",\"scan\":") + (g.scan ? L"true" : L"false");
    j += std::wstring(L",\"max\":") + (IsZoomed(g.hwnd) ? L"true" : L"false");
    std::wstring cur = a ? DisplayUrl(a) : L"";
    j += std::wstring(L",\"canStar\":") + (store::Recordable(cur) ? L"true" : L"false");
    j += std::wstring(L",\"starred\":") + (store::FindBookmark(cur) ? L"true" : L"false");
    j += std::wstring(L",\"find\":") + (g.findOpen ? L"true" : L"false");
    j += std::wstring(L",\"findCase\":") + (g.findCase ? L"true" : L"false");
    j += L",\"findText\":" + JsonStr(g.findText);
    j += L",\"search\":" + JsonStr(CurrentEngine().url);
    j += L"}";
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

static void SelectTab(int id, bool focus) {
    if (!FindTab(id)) return;
    if (id != g.active) HideFind();
    g.active = id;
    if (g.fsElement) { g.fsElement = false; UpdateFullscreen(); }
    g.hoverText.clear();
    Layout();
    SendState();
    InvalidateStatus();
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
    return s;
}

// at: position in the tab strip, or -1 for right after the active tab.
static void CreateTab(const std::wstring& url, bool activate, int at) {
    auto tab = std::make_unique<Tab>();
    int id = tab->id = g.nextId++;
    tab->url = url;
    int cur = IndexOf(g.active);
    int pos = at >= 0 ? std::min(at, (int)g.tabs.size()) : (cur < 0 ? (int)g.tabs.size() : cur + 1);
    g.tabs.insert(g.tabs.begin() + pos, std::move(tab));
    if (activate) { HideFind(); g.active = id; }

    g.creatingTab = id;
    CefRefPtr<CefBrowser> b = CefBrowserHost::CreateBrowserSync(ChildInfo(ContentRect()), MainClient(),
        url.empty() ? NEWTAB : url, BrowserSettings(), nullptr, nullptr);
    g.creatingTab = 0;
    Tab* t = FindTab(id);
    if (!b) { if (t) CloseTab(id); return; }
    if (t && !t->browser) t->browser = b;
    Layout();
    SendState();
    if (activate) FocusPage();
}

static void CloseTab(int id) {
    int i = IndexOf(id);
    if (i < 0) return;
    std::unique_ptr<Tab> t = std::move(g.tabs[i]);
    g.tabs.erase(g.tabs.begin() + i);
    if (!g.closing) {
        std::wstring u = DisplayUrl(t.get());
        if (!u.empty() && u.rfind(NEWTAB, 0) != 0 && u.rfind(L"data:", 0) != 0) {
            g.closed.push_back({ u, i });
            if (g.closed.size() > 25) g.closed.erase(g.closed.begin());
        }
    }
    if (t->browser) {
        HWND w = t->browser->GetHost()->GetWindowHandle();
        if (w) ShowWindow(w, SW_HIDE);
        t->browser->GetHost()->CloseBrowser(false);
    }
    if (g.tabs.empty()) { g.active = 0; CreateTab(NEWTAB, true); return; }
    if (g.active == id) SelectTab(g.tabs[std::min(i, (int)g.tabs.size() - 1)]->id, true);
    else SendState();
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

class DevToolsClient : public CefClient, public CefLifeSpanHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    void OnAfterCreated(CefRefPtr<CefBrowser>) override { g.devtools++; }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override { g.devtools--; MaybeFinishClose(); }
    IMPLEMENT_REFCOUNTING(DevToolsClient);
};

static std::vector<CefRefPtr<CefBrowser>> g_all;   // every live browser we own (for shutdown)
static size_t OpenBrowsers() { return g_all.size(); }

class Client : public CefClient, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler,
               public CefKeyboardHandler, public CefDownloadHandler, public CefContextMenuHandler,
               public CefRequestHandler, public CefFindHandler {
public:
    CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // --- life span ---
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString& target_url,
                       const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&,
                       CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>&, bool*) override {
        if (IsChrome(browser) || g.closing) return true;   // the chrome UI never opens popups
        // Keep the popup (so window.opener works for logins) but host it as a new tab.
        auto tab = std::make_unique<Tab>();
        int id = tab->id = g.nextId++;
        tab->url = target_url.ToWString();
        int at = IndexOf(g.active);
        if (at < 0) g.tabs.push_back(std::move(tab));
        else g.tabs.insert(g.tabs.begin() + at + 1, std::move(tab));
        HideFind();
        g.active = id;
        g.pendingPopups.push_back(id);
        windowInfo = ChildInfo(ContentRect());
        client = this;
        settings.background_color = CEF_BG;
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
        Tab* t = nullptr;
        if (g.creatingTab) t = FindTab(g.creatingTab);
        else if (!g.pendingPopups.empty()) { t = FindTab(g.pendingPopups.front()); g.pendingPopups.pop_front(); }
        if (t) t->browser = browser;
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
        else if (Tab* t = TabOf(browser)) {
            // closed from inside the page (window.close())
            t->browser = nullptr;
            if (!g.closing) CloseTab(t->id);
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
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (!frame->IsMain()) return;
        if (Tab* t = TabOf(browser)) {
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
        }
    }
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType) override {
        if (frame->IsMain() && TabOf(browser)) ApplyScan(browser);
    }
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                     const CefString&, const CefString& failedUrl) override {
        if (!frame->IsMain() || errorCode == ERR_ABORTED) return;
        Tab* t = TabOf(browser);
        if (!t) return;
        std::wstring url = failedUrl.ToWString();
        if (url.rfind(UI_HOST, 0) == 0 || url.rfind(L"data:", 0) == 0) return;
        t->errorUrl = url;
        t->errorPending = true;
        store::Forget(t->histId, url);   // failed loads stay out of the history
        t->histId = 0;
        frame->LoadURL(ErrorPageUrl(url, (int)errorCode));
    }

    // --- request ---
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus, int error_code, const CefString&) override {
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
            case 'H': if (!repeat) cmd = CMD_HISTORY; break;
            case 'J': if (!repeat) cmd = CMD_DOWNLOADS; break;
            case VK_OEM_COMMA: if (!repeat) cmd = CMD_SETTINGS; break;
            case 'O': if (shift && !repeat) cmd = CMD_BOOKMARKS; break;
            case 'W': case VK_F4: if (!repeat) cmd = CMD_CLOSETAB; break;
            case 'L': cmd = CMD_FOCUSURL; break;
            case 'R': cmd = CMD_RELOAD; break;
            case 'P': if (!repeat) cmd = CMD_PRINT; break;
            case VK_TAB: cmd = shift ? CMD_PREVTAB : CMD_NEXTTAB; break;
            case VK_OEM_PLUS: case VK_ADD: cmd = CMD_ZOOMIN; break;
            case VK_OEM_MINUS: case VK_SUBTRACT: cmd = CMD_ZOOMOUT; break;
            case '0': case VK_NUMPAD0: cmd = CMD_ZOOMRESET; break;
            default:
                if (vk >= '1' && vk <= '9' && !g.tabs.empty()) {
                    size_t i = (vk == '9') ? g.tabs.size() - 1 : (size_t)(vk - '1');
                    if (i < g.tabs.size()) { SelectTab(g.tabs[i]->id, true); return true; }
                }
            }
        } else if (alt && !ctrl) {
            if (vk == VK_LEFT) cmd = CMD_BACK;
            else if (vk == VK_RIGHT) cmd = CMD_FWD;
            else if (vk == VK_HOME) cmd = CMD_HOME;
        } else if (!ctrl && !alt) {
            if (vk == VK_F5) cmd = CMD_RELOAD;
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
        if (IsChrome(browser)) {
            if (!(params->GetTypeFlags() & CM_TYPEFLAG_EDITABLE)) model->Clear();
            return;
        }
        if (!params->GetLinkUrl().empty()) {
            model->InsertItemAt(0, MENU_OPEN_LINK_TAB, "Open link in new tab");
            model->InsertSeparatorAt(1);
        }
        if (model->GetCount() > 0) model->AddSeparator();
        model->AddItem(MENU_INSPECT, "Inspect");
    }
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params,
                              int command_id, EventFlags) override {
        if (command_id == MENU_OPEN_LINK_TAB) {
            std::wstring link = params->GetLinkUrl().ToWString();
            if (link.rfind(L"http", 0) == 0) CreateTab(link, false);
            return true;
        }
        if (command_id == MENU_INSPECT) {
            CefWindowInfo wi; wi.SetAsPopup(nullptr, "NeonNet DevTools");
            browser->GetHost()->ShowDevTools(wi, new DevToolsClient(), CefBrowserSettings(),
                                             CefPoint(params->GetXCoord(), params->GetYCoord()));
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
    o += std::string("],\"scan\":") + b(g.scan) + ",\"autoUpdate\":" + b(AutoUpdateEnabled()) + ",\"installed\":" + b(IsInstalledCopy()) +
         ",\"home\":" + JsonUtf8(RegGetString(L"HomePage")) + ",\"askDownload\":" + b(RegGetDword(L"AskDownload", 1) != 0) +
         ",\"downloadDir\":" + JsonUtf8(KnownFolder(FOLDERID_Downloads)) + ",\"version\":" + JsonUtf8(APP_VERSION) +
         ",\"chromium\":" + JsonUtf8(chromium) + "}";
    return o;
}

// https://neonnet.ui/api/* : history, bookmarks, downloads and settings for NeonNet's own
// pages. Only a main frame that shows a neonnet.ui page may call it, so websites can neither
// read nor change anything.
static CefRefPtr<CefResourceHandler> ServeApi(const std::wstring& path, const std::wstring& query) {
    std::string out = "{}";
    bool changed = false;
    long long id = _wtoi64(QueryParam(query, L"id").c_str());
    if (path == L"api/history") out = store::HistoryJson(QueryParam(query, L"q"), 1500);
    else if (path == L"api/history/delete") changed = store::DeleteHistory(id);
    else if (path == L"api/history/clear") { store::ClearHistory(); changed = true; }
    else if (path == L"api/bookmarks") out = store::BookmarksJson();
    else if (path == L"api/bookmarks/delete") changed = store::DeleteBookmark(id);
    else if (path == L"api/bookmarks/edit") {
        if (!store::EditBookmark(id, QueryParam(query, L"title"), QueryParam(query, L"url")))
            return new StringResource("{\"error\":\"invalid address\"}", "application/json", 400);
        changed = true;
    }
    else if (path == L"api/downloads") out = store::DownloadsJson();
    else if (path == L"api/downloads/remove") changed = store::DownloadRemove(id);
    else if (path == L"api/downloads/clear") { store::DownloadsClear(); changed = true; }
    else if (path == L"api/downloads/cancel" || path == L"api/downloads/open" || path == L"api/downloads/show") {
        // The page sends only an id; what to open is looked up here, never taken from the request.
        WPARAM act = path == L"api/downloads/cancel" ? DLA_CANCEL : path == L"api/downloads/open" ? DLA_OPEN : DLA_SHOW;
        PostMessageW(g.hwnd, WM_APP_DOWNLOAD, act, (LPARAM)id);
    }
    else if (path == L"api/settings") out = SettingsJson();
    else if (path == L"api/settings/set") {
        std::wstring key = QueryParam(query, L"key"), val = QueryParam(query, L"value");
        bool ok = false;
        if (key == L"search") { for (const auto& e : SEARCH_ENGINES) if (val == e.id) ok = true; }
        else if (key == L"scan" || key == L"autoUpdate" || key == L"askDownload") ok = (val == L"0" || val == L"1");
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
            if (browser && browser->GetIdentifier() == g.chromeId.load() && query.rfind(L"m=", 0) == 0) {
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
        if (path.rfind(L"api/", 0) == 0) {
            bool trusted = browser && browser->GetIdentifier() != g.chromeId.load() && frame && frame->IsMain() &&
                           frame->GetURL().ToWString().rfind(UI_HOST, 0) == 0;
            if (!trusted) return new StringResource("", "text/plain", 403);
            return ServeApi(path, query);
        }
        int id = 0;
        if (path == L"chrome.html") id = IDR_CHROME;
        else if (path == L"newtab.html" || path.empty()) id = IDR_NEWTAB;
        else if (path == L"history.html") id = IDR_HISTORY;
        else if (path == L"bookmarks.html") id = IDR_BOOKMARKS;
        else if (path == L"downloads.html") id = IDR_DOWNLOADS;
        else if (path == L"settings.html") id = IDR_SETTINGS;
        std::string body;
        if (id && LoadUi(id, body)) return new StringResource(std::move(body), "text/html", 200);
        return new StringResource("not found", "text/plain", 404);
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

static bool IsInstalledCopy() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\" + APP_DIR_NAME;
    return _wcsicmp(self, dir.c_str()) == 0;
}

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
    if (g.closing || g.upd == U_CHECKING || g.upd == U_DOWNLOADING) return;
    if (g.upd == U_READY) { if (manual) ShowNotice(L"UPDATE v" + g.updVersion + L" IS READY - USE THE MENU TO RESTART"); return; }
    if (g.updThread) { WaitForSingleObject(g.updThread, 0); CloseHandle(g.updThread); g.updThread = nullptr; }
    g.upd = U_CHECKING;
    if (manual) { g.notice = L"CHECKING FOR UPDATES..."; KillTimer(g.hwnd, TIMER_NOTICE); InvalidateStatus(); }
    int flags = (manual ? 1 : 0) | (IsInstalledCopy() ? 2 : 0);
    g.updThread = CreateThread(nullptr, 0, UpdateThread, (LPVOID)(INT_PTR)flags, 0, nullptr);
    if (!g.updThread) g.upd = U_IDLE;
}

static std::wstring Upper(std::wstring s) { for (auto& c : s) c = towupper(c); return s; }

static void OnUpdateMsg(const UpdateMsg& m) {
    switch (m.state) {
    case U_DOWNLOADING: g.upd = U_DOWNLOADING; g.updPercent = m.percent; g.updVersion = m.version; break;
    case U_READY: g.upd = U_READY; g.updVersion = m.version; g.updPath = m.path; g.notice.clear(); break;
    case U_AVAILABLE: g.upd = U_AVAILABLE; g.updVersion = m.version; g.updPage = m.page; g.notice.clear(); break;
    case U_UPTODATE:
        g.upd = U_IDLE;
        if (m.manual) ShowNotice(L"NEONNET " APP_VERSION L" IS UP TO DATE");
        break;
    case U_FAILED:
        g.upd = U_IDLE;
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
        L"  Ctrl+H  history\n"
        L"  Ctrl+L  address bar      Ctrl+Tab  next tab\n"
        L"  F5      reload           Alt+Left/Right  back/forward\n"
        L"  Ctrl +/-/0  zoom         F11  fullscreen\n"
        L"  Ctrl+P  print            F12  developer tools";
    MessageBoxW(g.hwnd, v.c_str(), L"About NeonNet", MB_OK | MB_ICONINFORMATION);
}

static void ToggleBookmark() {
    Tab* a = Active();
    if (!a) return;
    std::wstring u = DisplayUrl(a);
    if (!store::Recordable(u)) { ShowNotice(L"THIS PAGE CANNOT BE BOOKMARKED"); return; }
    if (long long id = store::FindBookmark(u)) { store::DeleteBookmark(id); ShowNotice(L"BOOKMARK REMOVED"); }
    else { store::AddBookmark(u, a->title); ShowNotice(L"BOOKMARKED: " + Upper(a->title.empty() ? u : a->title)); }
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

static void ExecCmd(int cmd) {
    Tab* a = Active();
    CefRefPtr<CefBrowser> b = a ? a->browser : nullptr;
    switch (cmd) {
    case CMD_NEWTAB: CreateTab(NEWTAB, true); break;
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
        int n = (int)g.tabs.size(), i = IndexOf(g.active);
        if (n > 1 && i >= 0) SelectTab(g.tabs[(i + (cmd == CMD_NEXTTAB ? 1 : n - 1)) % n]->id, true);
        break;
    }
    case CMD_ZOOMIN: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_IN); break;
    case CMD_ZOOMOUT: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_OUT); break;
    case CMD_ZOOMRESET: if (b) b->GetHost()->Zoom(CEF_ZOOM_COMMAND_RESET); break;
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
        if (b) { CefWindowInfo wi; wi.SetAsPopup(nullptr, "NeonNet DevTools"); b->GetHost()->ShowDevTools(wi, new DevToolsClient(), CefBrowserSettings(), CefPoint()); }
        break;
    case CMD_ABOUT: ShowAbout(); break;
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
            CreateTab(c.url, true, c.index);
        }
        break;
    case CMD_BOOKMARK: ToggleBookmark(); break;
    case CMD_BOOKMARKS: OpenPage(BOOKMARKS_PAGE); break;
    case CMD_HISTORY: OpenPage(HISTORY_PAGE); break;
    case CMD_DOWNLOADS: OpenPage(DOWNLOADS_PAGE); break;
    case CMD_SETTINGS: OpenPage(SETTINGS_PAGE); break;
    }
    // zoom changes are async; refresh the status bar shortly after
    if (cmd == CMD_ZOOMIN || cmd == CMD_ZOOMOUT || cmd == CMD_ZOOMRESET) SetTimer(g.hwnd, TIMER_ZOOM, 150, nullptr);
}

// ---------------------------------------------------------------------------
// The ≡ menu: a popup window painted in the NeonNet theme (a native menu cannot be
// recolored or given clipped corners). RunMenu() runs its own message loop, like
// TrackPopupMenu, and returns the chosen command or 0.

struct MenuItem {
    int cmd = 0;              // 0 = separator
    std::wstring text, keys;
    bool disabled = false, checked = false;
};

struct MenuState {
    std::vector<MenuItem> items;
    std::vector<RECT> rows;
    HFONT font = nullptr;
    int hot = -1, result = 0;
    bool done = false, tracking = false;
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
    DrawTextW(mem, L"> SYSTEM MENU", -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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
            SetTextColor(mem, hot ? C_BG : C_PINK);
            DrawTextW(mem, L"\x25A0", -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        RECT tr{ r.left + S(32), r.top, r.right - S(16), r.bottom };
        SetTextColor(mem, tc);
        DrawTextW(mem, it.text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(mem, kc);
        DrawTextW(mem, it.keys.c_str(), -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // scanlines, like the rest of the UI
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

    // frame: pink outline following the clipped corners (top-right and bottom-left)
    HPEN pink = CreatePen(PS_SOLID, 1, C_PINK);
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

// rightTop: screen point where the menu's top-right corner goes.
static int RunMenu(std::vector<MenuItem> items, POINT rightTop) {
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
    int x = std::max((int)mi.rcWork.left, (int)std::min(rightTop.x, mi.rcWork.right) - w);
    int top = std::min(rightTop.y, mi.rcWork.bottom - h);

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
    bool canStar = a && store::Recordable(DisplayUrl(a));
    add(CMD_NEWTAB, L"New tab", L"Ctrl+T");
    add(CMD_CLOSETAB, L"Close tab", L"Ctrl+W");
    add(CMD_REOPEN, L"Reopen closed tab", L"Ctrl+Shift+T", g.closed.empty());
    sep();
    add(CMD_BOOKMARK, starred ? L"Remove bookmark" : L"Bookmark this page", L"Ctrl+D", !canStar);
    add(CMD_BOOKMARKS, L"Bookmarks", L"Ctrl+Shift+O");
    add(CMD_HISTORY, L"History", L"Ctrl+H");
    add(CMD_DOWNLOADS, L"Downloads", L"Ctrl+J");
    add(CMD_FIND, L"Find in page", L"Ctrl+F");
    sep();
    add(CMD_ZOOMIN, L"Zoom in", L"Ctrl++");
    add(CMD_ZOOMOUT, L"Zoom out", L"Ctrl+-");
    add(CMD_ZOOMRESET, L"Reset zoom", L"Ctrl+0");
    sep();
    add(CMD_PRINT, L"Print", L"Ctrl+P");
    add(CMD_SCAN, L"Scanlines", L"", false, g.scan);
    add(CMD_FULLSCREEN, L"Fullscreen", L"F11", false, g.fullscreen);
    add(CMD_DEVTOOLS, L"Developer tools", L"F12");
    sep();
    add(CMD_SETTINGS, L"Settings", L"Ctrl+,");
    if (g.upd == U_READY) add(CMD_UPDATE_RESTART, L"Restart to update to v" + g.updVersion);
    if (g.upd == U_AVAILABLE) add(CMD_UPDATE_PAGE, L"Download v" + g.updVersion);
    add(CMD_UPDATE_CHECK, L"Check for updates", L"", g.upd == U_CHECKING || g.upd == U_DOWNLOADING);
    if (IsInstalledCopy()) add(CMD_UPDATE_AUTO, L"Automatic updates", L"", false, AutoUpdateEnabled());
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

static void OnChromeCommand(const std::wstring& msg) {
    size_t nl = msg.find(L'\n');
    std::wstring cmd = msg.substr(0, nl);
    std::wstring arg = nl == std::wstring::npos ? L"" : msg.substr(nl + 1);
    Tab* a = Active();
    if (cmd == L"ready") SendState();
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
    else if (cmd == L"newtab") ExecCmd(CMD_NEWTAB);
    else if (cmd == L"home") ExecCmd(CMD_HOME);
    else if (cmd == L"dropdown") {
        // address bar suggestions: the toolbar height they need (CSS px, 0 = closed)
        int n = std::max(0, std::min(_wtoi(arg.c_str()), 520));
        if (n != g.dropNeed) { g.dropNeed = n; Layout(); }
    }
    else if (cmd == L"close") CloseTab(_wtoi(arg.c_str()));
    else if (cmd == L"select") SelectTab(_wtoi(arg.c_str()), true);
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

class NeonApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> cl) override {
        if (!process_type.empty()) return;
        cl->AppendSwitch("hide-crash-restore-bubble");
        // Other software (download managers, antivirus) registers Chrome extensions system-wide;
        // Chromium would load them and open their pages as stray Chrome-style windows.
        cl->AppendSwitch("disable-extensions");
        cl->AppendSwitch("no-default-browser-check");
        cl->AppendSwitch("no-first-run");
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

        CefRegisterSchemeHandlerFactory("https", "neonnet.ui", new UiSchemeFactory());
        CefRegisterSchemeHandlerFactory("https", "chrome.neonnet.ui", new UiSchemeFactory());
        g_client = new Client();
        g.creatingChrome = true;
        CefBrowserHost::CreateBrowserSync(ChildInfo(ChromeRect()), g_client.get(), CHROME_UI, BrowserSettings(), nullptr, nullptr);
        g.creatingChrome = false;
        CreateTab(g.startUrl.empty() ? HomeUrl() : g.startUrl, true);
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

static void Paint(HDC hdc) {
    RECT rc; GetClientRect(g.hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(C_BG); FillRect(mem, &rc, bg); DeleteObject(bg);

    if (!g.fullscreen) {
        RECT st = StatusRect();
        HPEN pink = CreatePen(PS_SOLID, 1, C_PINK);
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
        if (!g.hoverText.empty()) { left = g.hoverText; lc = C_CYAN; }
        else if (a && a->loading) { left = L"LOADING..."; lc = C_YELLOW; }
        else if (!g.download.empty()) { left = g.download; lc = C_YELLOW; }
        else if (!g.notice.empty()) { left = g.notice; lc = C_CYAN; }
        else if (g.upd == U_DOWNLOADING) { left = L"DOWNLOADING UPDATE v" + g.updVersion + L"  " + std::to_wstring(g.updPercent) + L"%"; lc = C_YELLOW; }
        else if (g.upd == U_READY) { left = L"UPDATE v" + g.updVersion + L" READY - INSTALLS WHEN NEONNET CLOSES"; lc = C_YELLOW; }
        else if (g.upd == U_AVAILABLE) { left = L"NEONNET v" + g.updVersion + L" IS AVAILABLE - SEE THE MENU"; lc = C_YELLOW; }
        else if (a && !a->errorUrl.empty()) { left = L"SIGNAL LOST"; lc = C_PINK; }
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
    } else if (key == L"askDownload") {
        DWORD v = val == L"1" ? 1 : 0;
        RegSetBin(L"AskDownload", REG_DWORD, &v, sizeof(v));
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
    if (g.fullscreen) return;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(g.hwnd, &wp)) RegSetBin(L"Placement", REG_BINARY, &wp, sizeof(wp));
}

static void BeginClose() {
    if (g.closing) return;
    SavePlacement();
    store::SaveIfDirty();
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
    case WM_SIZE: Layout(); SendState(); return 0;
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
        if (wp == TIMER_ZOOM) KillTimer(hwnd, TIMER_ZOOM);
        else if (wp == TIMER_NOTICE) { KillTimer(hwnd, TIMER_NOTICE); g.notice.clear(); }
        else if (wp == TIMER_UPDATE) MaybeAutoCheck();
        else if (wp == TIMER_SAVE) { KillTimer(hwnd, TIMER_SAVE); store::SaveIfDirty(); return 0; }
        InvalidateStatus();
        return 0;
    case WM_APP_UPDATE: { std::unique_ptr<UpdateMsg> m((UpdateMsg*)lp); if (!g.closing) OnUpdateMsg(*m); return 0; }
    case WM_APP_STORE: ScheduleSave(); SendState(); return 0;
    case WM_APP_DOWNLOAD: DoDownloadAction((int)wp, (long long)lp); return 0;
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

static int Uninstall() {
    if (MessageBoxW(nullptr, L"Uninstall NeonNet from this computer?", L"NeonNet uninstall",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    bool wipe = MessageBoxW(nullptr, L"Also delete browsing data (history, cookies, saved logins)?",
                            L"NeonNet uninstall", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    DeleteFileW((KnownFolder(FOLDERID_Programs) + L"\\" + SHORTCUT_NAME).c_str());
    DeleteFileW((KnownFolder(FOLDERID_Desktop) + L"\\" + SHORTCUT_NAME).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_KEY);

    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    std::wstring installDir = local + L"\\Programs\\" + APP_DIR_NAME;
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    std::wstring script = L"/c timeout /t 3 /nobreak >nul";
    if (_wcsicmp(self, installDir.c_str()) == 0) script += L" & rmdir /s /q \"" + installDir + L"\"";
    script += L" & rmdir /s /q \"" + local + L"\\" + APP_DIR_NAME + L"\\Updates\"";
    if (wipe) script += L" & rmdir /s /q \"" + local + L"\\" + APP_DIR_NAME + L"\"";
    if (script.find(L"rmdir") != std::wstring::npos) {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        ShellExecuteW(nullptr, L"open", (std::wstring(sys) + L"\\cmd.exe").c_str(), script.c_str(), sys, SW_HIDE);
    }
    MessageBoxW(nullptr, L"NeonNet has been removed.", L"NeonNet uninstall", MB_OK | MB_ICONINFORMATION);
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

static int RunMain(HINSTANCE hinst, int nShow, void* sandbox_info) {
    CefMainArgs main_args(hinst);
    int code = CefExecuteProcess(main_args, nullptr, sandbox_info);
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
        if (argv[i][0] != L'-' && g.startUrl.empty()) g.startUrl = NormalizeArgUrl(argv[i]);
    }
    if (argv) LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    store::Load(KnownFolder(FOLDERID_LocalAppData) + L"\\" + APP_DIR_NAME);
    EnableDarkMenus();
    g.scan = RegGetDword(L"Scanlines", 1) != 0;
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
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"NeonNet", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
    // Must stay off: with session cookies persisted CEF switches Chromium to "restore last
    // session" and re-opens old tabs as stray Chrome-style windows on every start.
    settings.persist_session_cookies = false;
    settings.background_color = CEF_BG;
    settings.log_severity = LOGSEVERITY_DISABLE;

    CefRefPtr<NeonApp> app(new NeonApp());
    if (!CefInitialize(main_args, settings, app.get(), sandbox_info)) {
        // another NeonNet already owns the profile; it got our URL via OnAlreadyRunningAppRelaunch
        DestroyWindow(g.hwnd);
        return CefGetExitCode();
    }

    WINDOWPLACEMENT wp{ sizeof(wp) };
    DWORD sz = sizeof(wp);
    bool restored = false;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Placement", RRF_RT_REG_BINARY, nullptr, &wp, &sz) == ERROR_SUCCESS &&
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

    CefRunMessageLoop();
    g_client = nullptr;
    store::SaveIfDirty();
    // Start the installer before CefShutdown(): it waits for this process to exit, and it must
    // not depend on CefShutdown() returning normally.
    UpdateShutdown();
    CefShutdown();
    if (g.font) DeleteObject(g.font);
    CoUninitialize();
    return 0;
}

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance, LPWSTR, int nCmdShow, void* sandbox_info, cef_version_info_t*) {
    return RunMain(hInstance, nCmdShow, sandbox_info);
}
