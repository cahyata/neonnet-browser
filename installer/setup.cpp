// NeonNet-Setup.exe - installer for NeonNet. By default it installs for the current user only (no admin needed);
// the user can pick another folder, or install for all users of the computer (asks for administrator rights).
// The app folder is appended to this exe as a zip: [setup exe][zip][u64 zip size]["NEONPAK1"].
// It is extracted with Windows' built-in tar.exe into the chosen folder (default %LOCALAPPDATA%\Programs\NeonNet),
// then Start menu / desktop shortcuts, an "Apps & features" entry and the web browser registration are created.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <string>
#include <thread>
#include "..\src\resource.h"
#include "..\src\browserreg.h"

static const COLORREF C_BG = RGB(0x12, 0x06, 0x2c), C_BG2 = RGB(0x0a, 0x04, 0x20);
static const COLORREF C_PINK = RGB(0xff, 0x2e, 0x97), C_CYAN = RGB(0x00, 0xe5, 0xff);
static const COLORREF C_YELLOW = RGB(0xff, 0xd6, 0x00), C_MUTED = RGB(0x8a, 0x6f, 0xc0), C_TEXT = RGB(0xe8, 0xd8, 0xff);
static const wchar_t* UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NeonNet";
static const wchar_t* SHORTCUT = L"NeonNet.lnk";
static const UINT WM_DONE = WM_APP + 1;

enum Phase { READY, WORKING, DONE, FAILED };
enum Hit { H_NONE, H_DESKTOP, H_MACHINE, H_DEFAULT, H_LAUNCH, H_CHANGE, H_PRIMARY, H_CANCEL, H_CLOSE };
// Results of an install. They are also the exit code of the elevated helper process (see InstallElevated).
enum Err { E_OK = 0, E_RUNNING = 10, E_DAMAGED, E_TEMP, E_UNPACK, E_DIR, E_NOTAR, E_EXTRACT, E_DENIED, E_ELEVATE };

static HINSTANCE gInst;
static HWND gWnd;
static UINT gDpi = 96;
static HFONT gFont, gBig, gSmall;
static Phase gPhase = READY;
static bool gDesktop = true, gLaunch = true;
static bool gMachine = false;     // install for all users (HKLM, Program Files, common Start menu): needs administrator rights
static bool gDefault = false;     // open Windows' Default apps page when done
static bool gDirCustom = false;   // the user picked the folder: do not replace it when switching between this user and all users
static Hit gHover = H_NONE;
static std::wstring gMsg, gNote, gInstallDir, gExe;
static int gDots = 0;

static int S(int v) { return MulDiv(v, (int)gDpi, 96); }

static std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static const int W = 560, H = 424, CUT = 18;
static RECT R(int l, int t, int r, int b) { return { S(l), S(t), S(r), S(b) }; }
static RECT RcChange()  { return R(W - 190, 146, W - 40, 176); }
static RECT RcDesktop() { return R(40, 190, 300, 216); }
static RECT RcMachine() { return R(40, 220, W - 40, 246); }
static RECT RcDefault() { return R(40, 250, W - 40, 276); }
static RECT RcLaunch()  { return R(40, 280, 300, 306); }
static RECT RcPrimary() { return R(W - 190, H - 66, W - 40, H - 30); }
static RECT RcCancel()  { return R(W - 320, H - 66, W - 206, H - 30); }
static RECT RcClose()   { return R(W - 64, 10, W - 28, 40); }

static Hit HitTest(POINT p) {
    if (gPhase == WORKING) return H_NONE;
    if (gPhase == READY) {
        RECT a = RcDesktop(), b = RcLaunch(), d = RcCancel(), m = RcMachine(), df = RcDefault(), ch = RcChange();
        if (PtInRect(&a, p)) return H_DESKTOP;
        if (PtInRect(&m, p)) return H_MACHINE;
        if (PtInRect(&df, p)) return H_DEFAULT;
        if (PtInRect(&b, p)) return H_LAUNCH;
        if (PtInRect(&ch, p)) return H_CHANGE;
        if (PtInRect(&d, p)) return H_CANCEL;
    }
    RECT c = RcPrimary(), e = RcClose();
    if (PtInRect(&c, p)) return H_PRIMARY;
    if (PtInRect(&e, p)) return H_CLOSE;
    return H_NONE;
}

// --- where to install ------------------------------------------------------------
static std::wstring DefaultDir(bool machine) {
    return machine ? Known(FOLDERID_ProgramFiles) + L"\\NeonNet" : Known(FOLDERID_LocalAppData) + L"\\Programs\\NeonNet";
}

// The folder an earlier install registered ("InstallLocation" of its Uninstall entry), empty when there is none.
static std::wstring RegisteredDir(HKEY root) {
    wchar_t buf[MAX_PATH] = {}; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, UNINSTALL_KEY, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}

static bool IsElevated() {
    HANDLE t = nullptr; TOKEN_ELEVATION e{}; DWORD n = 0; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        r = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
        CloseHandle(t);
    }
    return r;
}

static bool DirEmpty(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return true;
    bool empty = true;
    do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) { empty = false; break; } } while (FindNextFileW(f, &fd));
    FindClose(f);
    return empty;
}

// The folder NeonNet goes into when the user chose `p`. Uninstalling removes this whole folder, so NeonNet never takes
// over a folder that holds other things: a folder that is not empty and has no NeonNet in it gets a "NeonNet" folder
// inside it. Empty string: not a usable path.
static std::wstring ResolveDir(std::wstring p) {
    while (!p.empty() && (p.back() == L' ' || p.back() == L'\t')) p.pop_back();
    if (p.size() == 3 && p[1] == L':' && p[2] == L'\\') return p + L"NeonNet";   // a drive root
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    bool drive = p.size() >= 4 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\';
    if (!drive || p.size() > 200) return L"";
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return p;                       // a new folder
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return L"";
    if (GetFileAttributesW((p + L"\\NeonNet.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return p;   // an install of NeonNet
    return DirEmpty(p) ? p : p + L"\\NeonNet";
}

static void SetDir(const std::wstring& dir) { gInstallDir = dir; gExe = dir + L"\\NeonNet.exe"; }

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

static const wchar_t* ErrText(int e) {
    switch (e) {
    case E_RUNNING: return L"NeonNet is running - close it and try again";
    case E_DAMAGED: return L"Setup is damaged (payload missing)";
    case E_TEMP: return L"Cannot write to the temp folder";
    case E_UNPACK: return L"Could not unpack (disk full?)";
    case E_DIR: return L"Cannot create the install folder";
    case E_NOTAR: return L"tar.exe not found (Windows 10 1803 or newer is required)";
    case E_EXTRACT: return L"Extracting files failed";
    case E_DENIED: return L"Administrator permission was not given";
    case E_ELEVATE: return L"Could not start the installer with administrator rights";
    default: return L"Install failed";
    }
}

// Runs on a worker thread (or in the elevated helper). gMachine says whether it is an install for all users.
static int Install() {
    if (!gExtractOnly && AppRunning()) return E_RUNNING;

    // locate the appended payload
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return E_DAMAGED;
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    char footer[16] = {}; DWORD rd = 0;
    LARGE_INTEGER pos; pos.QuadPart = size.QuadPart - 16;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    ReadFile(f, footer, 16, &rd, nullptr);
    ULONGLONG zipLen = *(ULONGLONG*)footer;
    if (rd != 16 || memcmp(footer + 8, "NEONPAK1", 8) != 0 || zipLen == 0 || zipLen > (ULONGLONG)size.QuadPart) {
        CloseHandle(f); return E_DAMAGED;
    }

    // copy the zip to %TEMP%
    wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
    std::wstring zip = std::wstring(tmpDir) + L"neonnet-payload.zip";
    HANDLE z = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (z == INVALID_HANDLE_VALUE) { CloseHandle(f); return E_TEMP; }
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
    if (!ok) { DeleteFileW(zip.c_str()); return E_UNPACK; }

    // extract with the tar.exe that ships with Windows 10/11
    int e = SHCreateDirectoryExW(nullptr, gInstallDir.c_str(), nullptr);
    if (e != ERROR_SUCCESS && e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) { DeleteFileW(zip.c_str()); return E_DIR; }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -x -f \"" + zip + L"\" -C \"" + gInstallDir + L"\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(zip.c_str()); return E_NOTAR;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(zip.c_str());
    if (code != 0 || GetFileAttributesW(gExe.c_str()) == INVALID_FILE_ATTRIBUTES) return E_EXTRACT;
    if (gExtractOnly) return E_OK;

    // shortcuts: the Start menu one always, the desktop one when chosen
    std::wstring programs = Known(gMachine ? FOLDERID_CommonPrograms : FOLDERID_Programs);
    std::wstring desk = Known(gMachine ? FOLDERID_PublicDesktop : FOLDERID_Desktop) + L"\\" + SHORTCUT;
    MakeShortcut(programs + L"\\" + SHORTCUT, gExe);
    if (gDesktop) MakeShortcut(desk, gExe); else DeleteFileW(desk.c_str());

    HKEY root = gMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    HKEY k;
    if (RegCreateKeyExW(root, UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
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
    browserreg::Register(root, gExe);   // NeonNet appears in Settings > Default apps (Windows leaves the choice to the user)
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return E_OK;
}

// An install for all users writes to places only an administrator may change: run this installer again with
// administrator rights (Windows asks the user) and wait for it. Its exit code is the Err of its Install().
static int InstallElevated() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args = L"--elevated --machine --dir=\"" + gInstallDir + L"\" --desktop=" + (gDesktop ? L"1" : L"0");
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = args.c_str(); sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? E_DENIED : E_ELEVATE;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = E_ELEVATE; GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
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
        if (!gNote.empty())
            Text(dc, gNote, R(40, 124, W - 40, 146), C_PINK, gSmall, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        else
            Text(dc, std::wstring(L"> NeonNet " APP_VERSION L" + bundled Chromium (~410 MB), ") + (gMachine ? L"all users." : L"this user only."),
                 R(40, 124, W - 40, 146), C_TEXT, gSmall, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, gInstallDir, R(40, 150, W - 200, 172), C_MUTED, gSmall, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        Button(dc, RcChange(), L"CHANGE FOLDER", false, gHover == H_CHANGE);
        Toggle(dc, RcDesktop(), gDesktop, L"Create desktop shortcut", gHover == H_DESKTOP);
        Toggle(dc, RcMachine(), gMachine, L"Install for all users (asks for admin rights)", gHover == H_MACHINE);
        Toggle(dc, RcDefault(), gDefault, L"Choose NeonNet as my default browser when done", gHover == H_DEFAULT);
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
        if (gMachine) Text(dc, L"> Windows may ask for administrator permission.", R(40, 226, W - 40, 248), C_MUTED, gSmall, DT_LEFT | DT_SINGLELINE);
    } else {
        Text(dc, gPhase == DONE ? L"> Find it in the Start menu as NeonNet." : L"> Something went wrong:", R(40, 124, W - 40, 146), gPhase == DONE ? C_TEXT : C_YELLOW, gSmall, DT_LEFT | DT_SINGLELINE);
        Text(dc, gMsg, R(40, 150, W - 40, 190), C_MUTED, gSmall, DT_LEFT | DT_WORDBREAK | DT_PATH_ELLIPSIS);
        Button(dc, RcPrimary(), gPhase == DONE ? (gDefault ? L"CHOOSE" : gLaunch ? L"LAUNCH" : L"FINISH") : L"RETRY", true, gHover == H_PRIMARY);
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
        int code = (gMachine && !IsElevated()) ? InstallElevated() : Install();
        gMsg = code == E_OK ? L"Installed to " + gInstallDir : ErrText(code);
        CoUninitialize();
        PostMessageW(gWnd, WM_DONE, code == E_OK ? 1 : 0, 0);
    }).detach();
}

// "Change folder": the system's folder picker. What is chosen goes through ResolveDir (see there).
static void ChooseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose the folder for NeonNet");
    std::wstring parent = gInstallDir;
    size_t cut = parent.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2) parent.resize(cut);
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&start)))) { dlg->SetFolder(start); start->Release(); }
    if (SUCCEEDED(dlg->Show(gWnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring dir = ResolveDir(path);
                if (dir.empty()) gNote = L"> Choose a folder on a drive (for example D:\\Apps).";
                else { SetDir(dir); gDirCustom = true; gNote.clear(); }
                CoTaskMemFree(path);
            }
            it->Release();
        }
    }
    dlg->Release();
}

static void OnClick(Hit h) {
    switch (h) {
    case H_DESKTOP: gDesktop = !gDesktop; break;
    case H_LAUNCH: gLaunch = !gLaunch; break;
    case H_DEFAULT: gDefault = !gDefault; break;
    case H_MACHINE:
        gMachine = !gMachine;
        if (!gDirCustom) SetDir(DefaultDir(gMachine));   // the usual place for the kind of install, unless the user chose one
        break;
    case H_CHANGE: ChooseFolder(); break;
    case H_CANCEL: case H_CLOSE: DestroyWindow(gWnd); return;
    case H_PRIMARY:
        if (gPhase == DONE) {
            if (gDefault) browserreg::OpenDefaultAppsSettings();
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

    // An earlier install decides where this one goes (so upgrading by hand keeps the folder and the kind of install).
    std::wstring oldMachine = RegisteredDir(HKEY_LOCAL_MACHINE), oldUser = RegisteredDir(HKEY_CURRENT_USER);
    if (!oldMachine.empty()) { gMachine = true; SetDir(oldMachine); gDirCustom = _wcsicmp(oldMachine.c_str(), DefaultDir(true).c_str()) != 0; }
    else if (!oldUser.empty()) { SetDir(oldUser); gDirCustom = _wcsicmp(oldUser.c_str(), DefaultDir(false).c_str()) != 0; }
    else SetDir(DefaultDir(false));

    bool silent = false, relaunch = false, elevated = false;
    std::wstring dirArg;   // --dir=<folder>
    DWORD waitPid = 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (wcsncmp(argv[i], L"--extract-to=", 13) == 0 && argv[i][13]) {
            gExtractOnly = true;
            SetDir(argv[i] + 13);
        }
        else if (_wcsicmp(argv[i], L"--silent") == 0) silent = true;
        else if (_wcsicmp(argv[i], L"--relaunch") == 0) relaunch = true;
        else if (wcsncmp(argv[i], L"--wait-pid=", 11) == 0) waitPid = wcstoul(argv[i] + 11, nullptr, 10);
        else if (_wcsicmp(argv[i], L"--elevated") == 0) elevated = true;   // the helper an install for all users starts (see InstallElevated)
        else if (_wcsicmp(argv[i], L"--machine") == 0) gMachine = true;
        else if (wcsncmp(argv[i], L"--dir=", 6) == 0 && argv[i][6]) dirArg = argv[i] + 6;
        else if (wcsncmp(argv[i], L"--desktop=", 10) == 0) gDesktop = argv[i][10] == L'1';
    }
    if (argv) LocalFree(argv);
    if (!dirArg.empty()) {
        // The same rule as the folder picker (see ResolveDir): never take over a folder that holds other things.
        // (The elevated helper gets a folder the first run has resolved already.)
        std::wstring dir = ResolveDir(dirArg);
        if (dir.empty()) { CoUninitialize(); return E_DIR; }
        SetDir(dir); gDirCustom = true;
    }
    if (gExtractOnly) {
        int code = Install();
        CoUninitialize();
        return code;
    }
    if (elevated) {
        // started by an install for all users: only a real administrator may do this
        int code = (gMachine && IsElevated()) ? Install() : E_DENIED;
        CoUninitialize();
        return code;
    }
    if (silent) {
        // Started by NeonNet's auto-updater: no UI. Wait for the browser to exit first. Only an install for this
        // user updates itself, so this never asks for administrator rights.
        if (gMachine) { gMachine = false; SetDir(oldUser.empty() ? DefaultDir(false) : oldUser); }
        if (waitPid) {
            if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) { WaitForSingleObject(p, 60000); CloseHandle(p); }
        }
        for (int i = 0; i < 60 && AppRunning(); i++) Sleep(500);   // Chromium helper processes
        // keep the user's earlier choice for the desktop shortcut
        gDesktop = GetFileAttributesW((Known(FOLDERID_Desktop) + L"\\" + SHORTCUT).c_str()) != INVALID_FILE_ATTRIBUTES;
        int code = Install();
        bool ok = code == E_OK;
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NeonNet", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            if (ok) RegDeleteValueW(k, L"UpdateResult");
            else SetStr(k, L"UpdateResult", ErrText(code));
            RegCloseKey(k);
        }
        if (ok && relaunch) ShellExecuteW(nullptr, L"open", gExe.c_str(), nullptr, gInstallDir.c_str(), SW_SHOWNORMAL);
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
