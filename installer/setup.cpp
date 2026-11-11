// NeonNet-Setup.exe - per-user installer for NeonNet (no admin needed).
// The app folder is appended to this exe as a zip: [setup exe][zip][u64 zip size]["NEONPAK1"].
// It is extracted with Windows' built-in tar.exe into %LOCALAPPDATA%\Programs\NeonNet,
// then Start menu / desktop shortcuts and an "Apps & features" entry are created.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <thread>
#include "..\src\resource.h"

static const COLORREF C_BG = RGB(0x12, 0x06, 0x2c), C_BG2 = RGB(0x0a, 0x04, 0x20);
static const COLORREF C_PINK = RGB(0xff, 0x2e, 0x97), C_CYAN = RGB(0x00, 0xe5, 0xff);
static const COLORREF C_YELLOW = RGB(0xff, 0xd6, 0x00), C_MUTED = RGB(0x8a, 0x6f, 0xc0), C_TEXT = RGB(0xe8, 0xd8, 0xff);
static const wchar_t* UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NeonNet";
static const wchar_t* SHORTCUT = L"NeonNet.lnk";
static const UINT WM_DONE = WM_APP + 1;

enum Phase { READY, WORKING, DONE, FAILED };
enum Hit { H_NONE, H_DESKTOP, H_LAUNCH, H_PRIMARY, H_CANCEL, H_CLOSE };

static HINSTANCE gInst;
static HWND gWnd;
static UINT gDpi = 96;
static HFONT gFont, gBig, gSmall;
static Phase gPhase = READY;
static bool gDesktop = true, gLaunch = true;
static Hit gHover = H_NONE;
static std::wstring gMsg, gInstallDir, gExe;
static int gDots = 0;

static int S(int v) { return MulDiv(v, (int)gDpi, 96); }

static std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static const int W = 560, H = 360, CUT = 18;
static RECT R(int l, int t, int r, int b) { return { S(l), S(t), S(r), S(b) }; }
static RECT RcDesktop() { return R(40, 196, 300, 222); }
static RECT RcLaunch()  { return R(40, 228, 300, 254); }
static RECT RcPrimary() { return R(W - 190, H - 66, W - 40, H - 30); }
static RECT RcCancel()  { return R(W - 320, H - 66, W - 206, H - 30); }
static RECT RcClose()   { return R(W - 64, 10, W - 28, 40); }

static Hit HitTest(POINT p) {
    if (gPhase == WORKING) return H_NONE;
    if (gPhase == READY) {
        RECT a = RcDesktop(), b = RcLaunch(), d = RcCancel();
        if (PtInRect(&a, p)) return H_DESKTOP;
        if (PtInRect(&b, p)) return H_LAUNCH;
        if (PtInRect(&d, p)) return H_CANCEL;
    }
    RECT c = RcPrimary(), e = RcClose();
    if (PtInRect(&c, p)) return H_PRIMARY;
    if (PtInRect(&e, p)) return H_CLOSE;
    return H_NONE;
}

// --- install ------------------------------------------------------------------
static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(gInstallDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    sl->SetDescription(L"NeonNet Browser");
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

static void SetStr(HKEY k, const wchar_t* n, const std::wstring& v) {
    RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void SetDw(HKEY k, const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

static ULONGLONG DirSize(const std::wstring& dir) {
    ULONGLONG total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSize(dir + L"\\" + fd.cFileName);
        else total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}

static bool FileLocked(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

// Running executables/DLLs cannot be opened exclusively, so a lock means NeonNet is open
// (this also covers an older single-exe NeonNet installed in the same folder).
static bool AppRunning() {
    return FileLocked(gInstallDir + L"\\NeonNet.exe") || FileLocked(gInstallDir + L"\\libcef.dll");
}

static bool gExtractOnly = false;   // --extract-to=<dir>: unpack only (no shortcuts/registry), for testing

// Runs on a worker thread.
static bool Install() {
    if (!gExtractOnly && AppRunning()) { gMsg = L"NeonNet is running - close it and try again"; return false; }

    // locate the appended payload
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { gMsg = L"Cannot read setup file"; return false; }
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    char footer[16] = {}; DWORD rd = 0;
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - 16;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    ReadFile(f, footer, 16, &rd, nullptr);
    ULONGLONG zipLen = *(ULONGLONG*)footer;
    if (rd != 16 || memcmp(footer + 8, "NEONPAK1", 8) != 0 || zipLen == 0 || zipLen > (ULONGLONG)size.QuadPart) {
        CloseHandle(f); gMsg = L"Setup is damaged (payload missing)"; return false;
    }

    // copy the zip to %TEMP%
    wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
    std::wstring zip = std::wstring(tmpDir) + L"neonnet-payload.zip";
    HANDLE z = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (z == INVALID_HANDLE_VALUE) { CloseHandle(f); gMsg = L"Cannot write to the temp folder"; return false; }
    pos.QuadPart = size.QuadPart - 16 - (LONGLONG)zipLen;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::string buf(1 << 20, '\0');
    ULONGLONG left = zipLen; bool ok = true;
    while (left && ok) {
        DWORD want = (DWORD)std::min<ULONGLONG>(left, buf.size()), got = 0, wr = 0;
        ok = ReadFile(f, buf.data(), want, &got, nullptr) && got == want && WriteFile(z, buf.data(), got, &wr, nullptr) && wr == got;
        left -= got;
    }
    CloseHandle(f); CloseHandle(z);
    if (!ok) { DeleteFileW(zip.c_str()); gMsg = L"Could not unpack (disk full?)"; return false; }

    // extract with the tar.exe that ships with Windows 10/11
    int e = SHCreateDirectoryExW(nullptr, gInstallDir.c_str(), nullptr);
    if (e != ERROR_SUCCESS && e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) { DeleteFileW(zip.c_str()); gMsg = L"Cannot create install folder"; return false; }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -x -f \"" + zip + L"\" -C \"" + gInstallDir + L"\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(zip.c_str()); gMsg = L"tar.exe not found (Windows 10 1803 or newer is required)"; return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(zip.c_str());
    if (code != 0 || GetFileAttributesW(gExe.c_str()) == INVALID_FILE_ATTRIBUTES) { gMsg = L"Extracting files failed"; return false; }
    if (gExtractOnly) return true;

    MakeShortcut(Known(FOLDERID_Programs) + L"\\" + SHORTCUT, gExe);
    std::wstring desk = Known(FOLDERID_Desktop) + L"\\" + SHORTCUT;
    if (gDesktop) MakeShortcut(desk, gExe); else DeleteFileW(desk.c_str());

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SetStr(k, L"DisplayName", L"NeonNet Browser");
        SetStr(k, L"DisplayVersion", APP_VERSION);
        SetStr(k, L"Publisher", APP_PUBLISHER);
        SetStr(k, L"DisplayIcon", gExe + L",0");
        SetStr(k, L"InstallLocation", gInstallDir);
        SetStr(k, L"UninstallString", L"\"" + gExe + L"\" --uninstall");
        SetDw(k, L"EstimatedSize", (DWORD)(DirSize(gInstallDir) / 1024));
        SetDw(k, L"NoModify", 1);
        SetDw(k, L"NoRepair", 1);
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    gMsg = L"Installed to " + gInstallDir;
    return true;
}

// --- painting -----------------------------------------------------------------
static void Fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void Frame(HDC dc, RECT r, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); HGDIOBJ o = SelectObject(dc, p); HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob); SelectObject(dc, o); DeleteObject(p);
}
static void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt) {
    SelectObject(dc, f); SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
}
static void Toggle(HDC dc, RECT r, bool on, const wchar_t* label, bool hover) {
    RECT box = { r.left, r.top + S(5), r.left + S(16), r.top + S(21) };
    Frame(dc, box, hover ? C_CYAN : C_MUTED);
    if (on) { RECT in = { box.left + S(4), box.top + S(4), box.right - S(4), box.bottom - S(4) }; Fill(dc, in, C_PINK); }
    RECT t = { box.right + S(10), r.top, r.right, r.bottom };
    Text(dc, label, t, hover ? C_CYAN : C_TEXT, gFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
static void Button(HDC dc, RECT r, const wchar_t* label, bool primary, bool hover) {
    if (primary) Fill(dc, r, hover ? C_CYAN : C_PINK);
    else Frame(dc, r, hover ? C_CYAN : C_MUTED);
    if (primary) {
        int c = S(8);
        POINT tri[3] = { {r.right - c, r.top}, {r.right, r.top}, {r.right, r.top + c} };
        HBRUSH b = CreateSolidBrush(C_BG); HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(b);
    }
    Text(dc, label, r, primary ? C_BG : (hover ? C_CYAN : C_MUTED), gFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void Paint(HDC hdc) {
    RECT rc; GetClientRect(gWnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, rc, C_BG);
    RECT hdr = { 0, 0, rc.right, S(50) };
    Fill(dc, hdr, C_BG2);
    HPEN cy = CreatePen(PS_SOLID, 1, C_CYAN); HGDIOBJ op = SelectObject(dc, cy);
    MoveToEx(dc, 0, hdr.bottom, nullptr); LineTo(dc, rc.right, hdr.bottom);
    SelectObject(dc, op); DeleteObject(cy);
    HPEN sl = CreatePen(PS_SOLID, 1, RGB(0x0c, 0x03, 0x1f)); op = SelectObject(dc, sl);
    for (int y = 2; y < rc.bottom; y += 3) if (y != hdr.bottom) { MoveToEx(dc, 1, y, nullptr); LineTo(dc, rc.right - 1, y); }
    SelectObject(dc, op); DeleteObject(sl);

    Text(dc, L"\x25A0 NEON//NET.OS  SETUP", R(24, 0, 460, 50), C_YELLOW, gFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (gPhase != WORKING)
        Text(dc, L"\x2715", RcClose(), gHover == H_CLOSE ? C_PINK : C_MUTED, gFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* title = gPhase == DONE ? L"INSTALL COMPLETE" : gPhase == FAILED ? L"INSTALL FAILED"
                         : gPhase == WORKING ? L"INSTALLING" : L"WELCOME TO 2099";
    Text(dc, title, R(40, 70, W - 40, 120), gPhase == FAILED ? C_PINK : C_CYAN, gBig, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (gPhase == READY) {
        Text(dc, L"> NeonNet " APP_VERSION L" + bundled Chromium (~410 MB), this user only.", R(40, 124, W - 40, 146), C_TEXT, gSmall, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, gInstallDir, R(40, 150, W - 40, 172), C_MUTED, gSmall, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        Toggle(dc, RcDesktop(), gDesktop, L"Create desktop shortcut", gHover == H_DESKTOP);
        Toggle(dc, RcLaunch(), gLaunch, L"Launch NeonNet when done", gHover == H_LAUNCH);
        Button(dc, RcCancel(), L"CANCEL", false, gHover == H_CANCEL);
        Button(dc, RcPrimary(), L"INSTALL", true, gHover == H_PRIMARY);
    } else if (gPhase == WORKING) {
        std::wstring dots(gDots % 4, L'.');
        Text(dc, L"> Unpacking Chromium runtime" + dots, R(40, 124, W - 40, 146), C_YELLOW, gSmall, DT_LEFT | DT_SINGLELINE);
        Text(dc, gInstallDir, R(40, 150, W - 40, 172), C_MUTED, gSmall, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        // indeterminate bar
        RECT bar = R(40, 200, W - 40, 212);
        Frame(dc, bar, C_CYAN);
        int bw = bar.right - bar.left, seg = bw / 4, x = bar.left + ((gDots * S(18)) % (bw + seg)) - seg;
        RECT fillr = { std::max<int>(bar.left + 2, x), bar.top + 2, std::min<int>(bar.right - 2, x + seg), bar.bottom - 2 };
        if (fillr.right > fillr.left) Fill(dc, fillr, C_PINK);
    } else {
        Text(dc, gPhase == DONE ? L"> Find it in the Start menu as NeonNet." : L"> Something went wrong:", R(40, 124, W - 40, 146), gPhase == DONE ? C_TEXT : C_YELLOW, gSmall, DT_LEFT | DT_SINGLELINE);
        Text(dc, gMsg, R(40, 150, W - 40, 190), C_MUTED, gSmall, DT_LEFT | DT_WORDBREAK | DT_PATH_ELLIPSIS);
        Button(dc, RcPrimary(), gPhase == DONE ? (gLaunch ? L"LAUNCH" : L"FINISH") : L"RETRY", true, gHover == H_PRIMARY);
    }

    HPEN pk = CreatePen(PS_SOLID, 1, RGB(0x5a, 0x16, 0x70)); op = SelectObject(dc, pk);
    int fy = S(H - 90);
    for (int i = 0; i < 4; i++) { int y = fy + S(i * i * 5 + i * 4); MoveToEx(dc, S(24), y, nullptr); LineTo(dc, S(200), y); }
    SelectObject(dc, op); DeleteObject(pk);

    int c = S(CUT), w = rc.right, h = rc.bottom;
    HPEN pp = CreatePen(PS_SOLID, 1, C_PINK); op = SelectObject(dc, pp);
    POINT pts[7] = { {0, 0}, {w - c - 1, 0}, {w - 1, c}, {w - 1, h - 1}, {c, h - 1}, {0, h - c - 1}, {0, 0} };
    Polyline(dc, pts, 7);
    SelectObject(dc, op); DeleteObject(pp);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
}

static void Fonts() {
    for (HFONT* f : { &gFont, &gBig, &gSmall }) if (*f) DeleteObject(*f);
    gFont = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    gSmall = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    gBig = CreateFontW(-S(38), 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Impact");
}

static void ApplyRegion() {
    RECT wr; GetWindowRect(gWnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top, c = S(CUT);
    POINT pts[6] = { {0, 0}, {w - c, 0}, {w, c}, {w, h}, {c, h}, {0, h - c} };
    SetWindowRgn(gWnd, CreatePolygonRgn(pts, 6, WINDING), TRUE);
}

static void StartInstall() {
    gPhase = WORKING;
    SetTimer(gWnd, 1, 120, nullptr);
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = Install();
        CoUninitialize();
        PostMessageW(gWnd, WM_DONE, ok ? 1 : 0, 0);
    }).detach();
}

static void OnClick(Hit h) {
    switch (h) {
    case H_DESKTOP: gDesktop = !gDesktop; break;
    case H_LAUNCH: gLaunch = !gLaunch; break;
    case H_CANCEL: case H_CLOSE: DestroyWindow(gWnd); return;
    case H_PRIMARY:
        if (gPhase == DONE) {
            if (gLaunch) ShellExecuteW(nullptr, L"open", gExe.c_str(), nullptr, gInstallDir.c_str(), SW_SHOWNORMAL);
            DestroyWindow(gWnd);
            return;
        }
        StartInstall();
        break;
    default: return;
    }
    InvalidateRect(gWnd, nullptr, FALSE);
}

static LRESULT CALLBACK Proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &p);
        return HitTest(p) == H_NONE ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Hit h = HitTest(p);
        if (h != gHover) { gHover = h; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: gHover = H_NONE; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONUP: { POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; OnClick(HitTest(p)); return 0; }
    case WM_KEYDOWN:
        if (gPhase == WORKING) return 0;
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_RETURN) OnClick(H_PRIMARY);
        return 0;
    case WM_TIMER: gDots++; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_DONE:
        KillTimer(hwnd, 1);
        gPhase = wp ? DONE : FAILED;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE: if (gPhase == WORKING) return 0; break;
    case WM_DPICHANGED: {
        gDpi = HIWORD(wp); Fonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyRegion();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); Paint(dc); EndPaint(hwnd, &ps); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gInst = hi;
    gInstallDir = Known(FOLDERID_LocalAppData) + L"\\Programs\\NeonNet";
    gExe = gInstallDir + L"\\NeonNet.exe";

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (wcsncmp(argv[i], L"--extract-to=", 13) == 0 && argv[i][13]) {
            gExtractOnly = true;
            gInstallDir = argv[i] + 13;
            gExe = gInstallDir + L"\\NeonNet.exe";
        }
    }
    if (argv) LocalFree(argv);
    if (gExtractOnly) {
        bool ok = Install();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = Proc; wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"NeonNetSetup";
    RegisterClassExW(&wc);

    gDpi = GetDpiForSystem();
    MONITORINFO mi{ sizeof(mi) };
    POINT cur; GetCursorPos(&cur);
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = S(W), h = S(H);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    gWnd = CreateWindowExW(0, wc.lpszClassName, L"NeonNet Setup", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                           x, y, w, h, nullptr, nullptr, hi, nullptr);
    UINT real = GetDpiForWindow(gWnd);
    if (real != gDpi) { gDpi = real; SetWindowPos(gWnd, nullptr, 0, 0, S(W), S(H), SWP_NOMOVE | SWP_NOZORDER); }
    Fonts();
    ApplyRegion();
    ShowWindow(gWnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return 0;
}
