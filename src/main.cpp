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
#include <windowsx.h>   // after CEF headers: its macros (GetFirstChild...) clash with cef_dom.h

// ---------------------------------------------------------------------------
// Constants

static const int BORDER_DIP = 5;
static const int CHROME_DIP = 84;
static const int STATUS_DIP = 24;
static const int CUT_DIP = 16;

static const COLORREF C_BG = RGB(0x12, 0x06, 0x2c);
static const COLORREF C_PINK = RGB(0xff, 0x2e, 0x97);
static const COLORREF C_CYAN = RGB(0x00, 0xe5, 0xff);
static const COLORREF C_YELLOW = RGB(0xff, 0xd6, 0x00);
static const COLORREF C_MUTED = RGB(0x8a, 0x6f, 0xc0);
static const cef_color_t CEF_BG = 0xFF12062C;

static const wchar_t* UI_HOST = L"https://neonnet.ui/";
static const wchar_t* NEWTAB = L"https://neonnet.ui/newtab.html";
static const wchar_t* CHROME_UI = L"https://neonnet.ui/chrome.html";
static const wchar_t* APP_DIR_NAME = L"NeonNet";
static const wchar_t* REG_KEY = L"Software\\NeonNet";
static const wchar_t* UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NeonNet";
static const wchar_t* SHORTCUT_NAME = L"NeonNet.lnk";

static const UINT WM_APP_CMD = WM_APP + 1;      // lParam = new std::wstring (chrome UI command)
static const UINT WM_APP_OPEN = WM_APP + 2;     // lParam = new std::wstring (url from a 2nd instance)

enum Cmd {
    CMD_NEWTAB = 1, CMD_CLOSETAB, CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SCAN,
    CMD_FULLSCREEN, CMD_DEVTOOLS, CMD_ABOUT, CMD_RELOAD, CMD_STOP, CMD_BACK, CMD_FWD,
    CMD_FOCUSURL, CMD_NEXTTAB, CMD_PREVTAB, CMD_HOME, CMD_PRINT
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
};

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
    bool scan = true;
    bool fsUser = false, fsElement = false, fullscreen = false;
    WINDOWPLACEMENT fsPrev{ sizeof(WINDOWPLACEMENT) };
    std::wstring hoverText, download;
    std::wstring startUrl;
    bool closing = false;
    int devtools = 0;
};
static App g;

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
static RECT ChromeRect() { RECT rc; GetClientRect(g.hwnd, &rc); int b = Frame(); return { b, b, rc.right - b, b + S(CHROME_DIP) }; }
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

static void Place(CefRefPtr<CefBrowser> b, const RECT& r, bool visible) {
    if (!b) return;
    HWND w = b->GetHost()->GetWindowHandle();
    if (!w) return;
    bool wasVisible = IsWindowVisible(w) != FALSE;
    if (visible) {
        SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!wasVisible) b->GetHost()->WasHidden(false);
    } else if (wasVisible) {
        ShowWindow(w, SW_HIDE);
        b->GetHost()->WasHidden(true);
    }
}

static void Layout() {
    if (!g.hwnd) return;
    if (g.chrome) Place(g.chrome, ChromeRect(), !g.fullscreen);
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
static void CreateTab(const std::wstring& url, bool activate);
static void CloseTab(int id);
static CefRefPtr<CefClient> MainClient();

static void SelectTab(int id, bool focus) {
    if (!FindTab(id)) return;
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

static void CreateTab(const std::wstring& url, bool activate) {
    auto tab = std::make_unique<Tab>();
    int id = tab->id = g.nextId++;
    tab->url = url;
    int at = IndexOf(g.active);
    if (at < 0) g.tabs.push_back(std::move(tab));
    else g.tabs.insert(g.tabs.begin() + at + 1, std::move(tab));
    if (activate) g.active = id;

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
        DestroyWindow(g.hwnd);
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
               public CefRequestHandler {
public:
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

    // --- display ---
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
        if (Tab* t = TabOf(browser)) {
            t->title = title.ToWString();
            if (t->title.rfind(L"data:text/html", 0) == 0) t->title = L"SIGNAL LOST";
            SendState();
        }
    }
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (!frame->IsMain()) return;
        if (Tab* t = TabOf(browser)) {
            t->url = url.ToWString();
            if (t->errorPending) t->errorPending = false;
            else t->errorUrl.clear();
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
            case 'T': if (!repeat) cmd = CMD_NEWTAB; break;
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
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString&,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override {
        callback->Continue("", true);   // ask where to save
        return true;
    }
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback>) override {
        std::wstring name = item->GetFullPath().ToWString();
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
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
        response->SetStatusText(status_ == 200 ? "OK" : status_ == 204 ? "No Content" : "Not Found");
        response->SetMimeType(mime_);
        response->SetCharset("utf-8");
        CefResponse::HeaderMap h;
        h.insert(std::make_pair("Cache-Control", "no-store"));
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

class UiSchemeFactory : public CefSchemeHandlerFactory {
public:
    // Runs on the IO thread.
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, const CefString&,
                                         CefRefPtr<CefRequest> request) override {
        std::wstring url = request->GetURL().ToWString();
        std::wstring path = url.substr(wcslen(UI_HOST));
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
        int id = 0;
        if (path == L"chrome.html") id = IDR_CHROME;
        else if (path == L"newtab.html" || path.empty()) id = IDR_NEWTAB;
        std::string body;
        if (id && LoadUi(id, body)) return new StringResource(std::move(body), "text/html", 200);
        return new StringResource("not found", "text/plain", 404);
    }
    IMPLEMENT_REFCOUNTING(UiSchemeFactory);
};

// ---------------------------------------------------------------------------
// Commands

static void ShowAbout() {
    std::wstring v = L"NEON//NET  v" APP_VERSION L"\n\n"
        L"Chromium " + std::to_wstring(cef_version_info(4)) + L"." + std::to_wstring(cef_version_info(5)) + L"." +
        std::to_wstring(cef_version_info(6)) + L"." + std::to_wstring(cef_version_info(7)) +
        L" (bundled, via the Chromium Embedded Framework).\nThird-party licenses: CREDITS.html in the install folder.\n\n"
        L"Shortcuts\n"
        L"  Ctrl+T  new tab          Ctrl+W  close tab\n"
        L"  Ctrl+L  address bar      Ctrl+Tab  next tab\n"
        L"  F5      reload           Alt+Left/Right  back/forward\n"
        L"  Ctrl +/-/0  zoom         F11  fullscreen\n"
        L"  Ctrl+P  print            F12  developer tools";
    MessageBoxW(g.hwnd, v.c_str(), L"About NeonNet", MB_OK | MB_ICONINFORMATION);
}

static void ExecCmd(int cmd) {
    Tab* a = Active();
    CefRefPtr<CefBrowser> b = a ? a->browser : nullptr;
    switch (cmd) {
    case CMD_NEWTAB: CreateTab(NEWTAB, true); break;
    case CMD_CLOSETAB: if (a) CloseTab(a->id); break;
    case CMD_HOME: if (b) b->GetMainFrame()->LoadURL(NEWTAB); break;
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
    }
    // zoom changes are async; refresh the status bar shortly after
    if (cmd == CMD_ZOOMIN || cmd == CMD_ZOOMOUT || cmd == CMD_ZOOMRESET) SetTimer(g.hwnd, 2, 150, nullptr);
}

static void ShowMenu(int cssX, int cssY) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_NEWTAB, L"New tab\tCtrl+T");
    AppendMenuW(m, MF_STRING, CMD_CLOSETAB, L"Close tab\tCtrl+W");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ZOOMIN, L"Zoom in\tCtrl++");
    AppendMenuW(m, MF_STRING, CMD_ZOOMOUT, L"Zoom out\tCtrl+-");
    AppendMenuW(m, MF_STRING, CMD_ZOOMRESET, L"Reset zoom\tCtrl+0");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_PRINT, L"Print...\tCtrl+P");
    AppendMenuW(m, MF_STRING | (g.scan ? MF_CHECKED : 0), CMD_SCAN, L"Scanlines");
    AppendMenuW(m, MF_STRING | (g.fullscreen ? MF_CHECKED : 0), CMD_FULLSCREEN, L"Fullscreen\tF11");
    AppendMenuW(m, MF_STRING, CMD_DEVTOOLS, L"Developer tools\tF12");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ABOUT, L"About NeonNet");
    RECT c = ChromeRect();
    POINT p{ c.left + S(cssX), c.top + S(cssY) };
    ClientToScreen(g.hwnd, &p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, p.x + S(32), p.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
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
    else if (cmd == L"close") CloseTab(_wtoi(arg.c_str()));
    else if (cmd == L"select") SelectTab(_wtoi(arg.c_str()), true);
    else if (cmd == L"scan") ExecCmd(CMD_SCAN);
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
        size_t sp = arg.find(L'\n');
        ShowMenu(_wtoi(arg.substr(0, sp).c_str()), sp == std::wstring::npos ? 0 : _wtoi(arg.substr(sp + 1).c_str()));
    }
}

// ---------------------------------------------------------------------------
// Application: browser-process callbacks

static std::wstring NormalizeArgUrl(std::wstring u) {
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
        cl->AppendSwitch("no-default-browser-check");
        cl->AppendSwitch("no-first-run");
    }

    void OnContextInitialized() override {
        // Never restore the previous session (it would come back as untracked Chrome windows).
        CefRefPtr<CefValue> startup = CefValue::Create();
        startup->SetInt(5);   // SessionStartupPref::kPrefValueNewTab
        CefString err;
        CefRequestContext::GetGlobalContext()->SetPreference("session.restore_on_startup", startup, err);

        CefRegisterSchemeHandlerFactory("https", "neonnet.ui", new UiSchemeFactory());
        g_client = new Client();
        g.creatingChrome = true;
        CefBrowserHost::CreateBrowserSync(ChildInfo(ChromeRect()), g_client.get(), CHROME_UI, BrowserSettings(), nullptr, nullptr);
        g.creatingChrome = false;
        CreateTab(g.startUrl.empty() ? NEWTAB : g.startUrl, true);
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

static void SavePlacement() {
    if (g.fullscreen) return;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(g.hwnd, &wp)) RegSetBin(L"Placement", REG_BINARY, &wp, sizeof(wp));
}

static void BeginClose() {
    if (g.closing) return;
    SavePlacement();
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
        if (wp == 2) KillTimer(hwnd, 2);
        InvalidateStatus();
        return 0;
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
    EnableDarkMenus();
    g.scan = RegGetDword(L"Scanlines", 1) != 0;

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
    SetTimer(g.hwnd, 1, 15000, nullptr);

    CefRunMessageLoop();
    g_client = nullptr;
    CefShutdown();
    if (g.font) DeleteObject(g.font);
    CoUninitialize();
    return 0;
}

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance, LPWSTR, int nCmdShow, void* sandbox_info, cef_version_info_t*) {
    return RunMain(hInstance, nCmdShow, sandbox_info);
}
