#pragma once
// Registers NeonNet with Windows as a web browser: the URL (http, https) and file (.htm, .html ...) handlers and the
// entry in Settings > Default apps. Shared by the app (the "Set as default browser" button) and the installer.
//
// Windows 10 and 11 do not let a program make itself the default browser: the registration only makes NeonNet
// appear in the list, and the user picks it there. The same keys are written under HKCU (an install for this user
// only) or HKLM (an install for all users).

#include <windows.h>
#include <shellapi.h>
#include <string>

namespace browserreg {

static const wchar_t* PROG_HTML = L"NeonNetHTML";   // ProgId for web documents
static const wchar_t* PROG_URL = L"NeonNetURL";     // ProgId for web addresses (what Windows reports for http and https)
static const wchar_t* APP_NAME = L"NeonNet";        // name under RegisteredApplications and StartMenuInternet

inline bool Put(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

inline std::wstring CapabilitiesKey() { return std::wstring(L"Software\\Clients\\StartMenuInternet\\") + APP_NAME + L"\\Capabilities"; }

// root: HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE. exe: full path of NeonNet.exe.
inline bool Register(HKEY root, const std::wstring& exe) {
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    const std::wstring icon = exe + L",0";
    bool ok = true;
    for (const wchar_t* prog : { PROG_HTML, PROG_URL }) {
        std::wstring k = std::wstring(L"Software\\Classes\\") + prog;
        ok &= Put(root, k, nullptr, prog == PROG_HTML ? L"NeonNet HTML Document" : L"NeonNet URL");
        ok &= Put(root, k + L"\\DefaultIcon", nullptr, icon);
        ok &= Put(root, k + L"\\shell\\open\\command", nullptr, command);
        if (prog == PROG_URL) ok &= Put(root, k, L"URL Protocol", L"");
    }
    std::wstring app = std::wstring(L"Software\\Clients\\StartMenuInternet\\") + APP_NAME;
    ok &= Put(root, app, nullptr, L"NeonNet");
    ok &= Put(root, app + L"\\DefaultIcon", nullptr, icon);
    ok &= Put(root, app + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\"");
    std::wstring cap = CapabilitiesKey();
    ok &= Put(root, cap, L"ApplicationName", L"NeonNet");
    ok &= Put(root, cap, L"ApplicationDescription", L"NeonNet, a synthwave web browser built on Chromium");
    ok &= Put(root, cap, L"ApplicationIcon", icon);
    for (const wchar_t* ext : { L".htm", L".html", L".shtml", L".xht", L".xhtml" }) ok &= Put(root, cap + L"\\FileAssociations", ext, PROG_HTML);
    for (const wchar_t* sc : { L"http", L"https" }) ok &= Put(root, cap + L"\\URLAssociations", sc, PROG_URL);
    ok &= Put(root, L"Software\\RegisteredApplications", APP_NAME, cap);
    return ok;
}

inline void Unregister(HKEY root) {
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\") + PROG_HTML).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\") + PROG_URL).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Clients\\StartMenuInternet\\") + APP_NAME).c_str());
    HKEY k;
    if (RegOpenKeyExW(root, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, APP_NAME);
        RegCloseKey(k);
    }
}

// Whether the user has chosen NeonNet for web links (what Settings > Default apps stores for http and https).
inline bool IsDefault() {
    for (const wchar_t* sc : { L"http", L"https" }) {
        wchar_t prog[128] = {}; DWORD sz = sizeof(prog);
        std::wstring k = std::wstring(L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\") + sc + L"\\UserChoice";
        if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), L"ProgId", RRF_RT_REG_SZ, nullptr, prog, &sz) != ERROR_SUCCESS) return false;
        if (wcscmp(prog, PROG_URL) != 0) return false;
    }
    return true;
}

// Opens Windows' own page where the user chooses the default browser (the page of NeonNet, where Windows has it).
inline void OpenDefaultAppsSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=NeonNet", nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace browserreg
