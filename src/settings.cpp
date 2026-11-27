#include "settings.h"
#include "json.h"
#include "presets.h"
#include <atomic>
#include <cwctype>
#include <mutex>
#include <utility>

namespace settings {
namespace {

const unsigned long FOLLOW_SYSTEM = 2;   // stored value: the user has not chosen, Windows decides

enum class Kind { Flag, Number, Text };
using NumberRule = bool (*)(int);
using TextRule = bool (*)(const std::wstring&);

struct Row {
    Id id;
    const wchar_t* page;     // name on the settings page and in /api/settings
    const wchar_t* store;    // name in storage
    Kind kind;
    bool incognitoSaves;
    int number;              // default of a flag or number
    const wchar_t* text;     // default of a text
    NumberRule numberOk;
    TextRule textOk;
};

constexpr Row OnOff(Id id, const wchar_t* page, const wchar_t* store, bool def, bool incognitoSaves = true) {
    return { id, page, store, Kind::Flag, incognitoSaves, def ? 1 : 0, L"", nullptr, nullptr };
}
constexpr Row Num(Id id, const wchar_t* page, const wchar_t* store, int def, NumberRule ok) {
    return { id, page, store, Kind::Number, true, def, L"", ok, nullptr };
}
constexpr Row Txt(Id id, const wchar_t* page, const wchar_t* store, const wchar_t* def, TextRule ok, bool incognitoSaves = true) {
    return { id, page, store, Kind::Text, incognitoSaves, 0, def, nullptr, ok };
}

bool HomeOk(const std::wstring& v) {
    if (v.empty()) return true;   // the new tab page
    if (v.size() >= 500) return false;
    std::wstring low = v;
    for (auto& c : low) c = (wchar_t)towlower(c);
    return low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0 || low.rfind(L"file:///", 0) == 0;
}
bool DohCustomOk(const std::wstring& v) {
    if (v.empty()) return true;
    if (v.size() >= 300 || v.size() <= 8 || v.rfind(L"https://", 0) != 0) return false;
    for (wchar_t c : v) if (c <= 0x20 || c == 0x7f) return false;
    return true;
}
bool DohProviderOk(const std::wstring& v) {
    for (const auto& p : DOH_PROVIDERS) if (v == p.id) return true;
    return false;
}
const ThemePreset* FindPreset(const std::wstring& id) {
    for (const auto& p : THEME_PRESETS) if (id == p.id) return &p;
    return nullptr;
}
bool PresetOk(const std::wstring& v) { return v == L"custom" || FindPreset(v); }
bool ColorOk(const std::wstring& v) { COLORREF c; std::wstring n; return ParseHexColor(v, c, n); }

// In the order of Id.
constexpr Row ROWS[] = {
    OnOff(Id::Scanlines,        L"scan",             L"Scanlines",        true),
    OnOff(Id::AutoUpdate,       L"autoUpdate",       L"AutoUpdate",       true),
    OnOff(Id::AskDownload,      L"askDownload",      L"AskDownload",      false),
    OnOff(Id::FastDownload,     L"fastDownload",     L"FastDownload",     true),
    OnOff(Id::DlSubfolder,      L"dlSubfolder",      L"DlSubfolder",      true),
    OnOff(Id::DlCategories,     L"dlCategories",     L"DlCategories",     true),
    OnOff(Id::RestoreSession,   L"restoreSession",   L"RestoreSession",   false),
    OnOff(Id::Favicons,         L"favicons",         L"ShowFavicons",     true),
    OnOff(Id::BookmarksBar,     L"bookmarksBar",     L"BookmarksBar",     true),
    OnOff(Id::HttpsOnly,        L"httpsOnly",        L"HttpsOnly",        false),
    OnOff(Id::BlockAds,         L"blockAds",         L"BlockAds",         true),
    OnOff(Id::BlockTrackers,    L"blockTrackers",    L"BlockTrackers",    true),
    OnOff(Id::StripTracking,    L"stripTracking",    L"StripTracking",    true),
    OnOff(Id::Block3pCookies,   L"block3pCookies",   L"Block3pCookies",   false),
    OnOff(Id::StrictReferrer,   L"strictReferrer",   L"StrictReferrer",   false),
    OnOff(Id::SavePasswords,    L"savePasswords",    L"SavePasswords",    true),
    OnOff(Id::A11yTree,         L"a11yTree",         L"Accessibility",    false),
    OnOff(Id::ReducedMotion,    L"reducedMotion",    L"ReducedMotion",    false),   // follows Windows
    OnOff(Id::HighContrast,     L"highContrast",     L"HighContrast",     false),   // follows Windows
    OnOff(Id::WelcomeDismissed, L"welcomeDismissed", L"WelcomeDismissed", false, false),
    OnOff(Id::SmoothScroll,     L"smoothScroll",     L"SmoothScroll",     true),
    OnOff(Id::NetSpeed,         L"netSpeed",         L"NetSpeed",         true),
    OnOff(Id::ForceDark,        L"forceDark",        L"ForceDark",        false),
    OnOff(Id::LightMode,        L"lightMode",        L"LightMode",        false, false),
    OnOff(Id::BgLimit,          L"bgLimit",          L"BgLimit",          false),
    OnOff(Id::BatterySaver,     L"batterySaver",     L"BatterySaver",     false),
    OnOff(Id::BatteryAuto,      L"batteryAuto",      L"BatteryAuto",      false),
    OnOff(Id::NetLimitAll,      L"netLimitAll",      L"NetLimitAll",      false),

    Num(Id::MinFontSize,     L"minFontSize",     L"MinFontSize",     0,   [](int v) { return v == 0 || v == 12 || v == 14 || v == 16 || v == 18 || v == 20; }),
    Num(Id::FastConnections, L"fastConnections", L"FastConnections", 8,   [](int v) { return v == 4 || v == 8 || v == 16; }),
    Num(Id::RamLimitMB,      L"ramLimitMB",      L"RamLimitMB",      0,   [](int v) { return v == 0 || (v >= 512 && v <= 65536); }),
    Num(Id::NetLimitKB,      L"netLimitKB",      L"NetLimitKB",      0,   [](int v) { return v == 0 || (v >= 64 && v <= 102400); }),
    Num(Id::DockDefault,     L"dockDefault",     L"DockDefault",     0,   [](int v) { return v <= 2; }),
    Num(Id::DockWidth,       L"dockWidth",       L"DockWidth",       500, [](int v) { return v >= 300 && v <= 900; }),
    Num(Id::ReaderSize,      L"readerSize",      L"ReaderSize",      18,  [](int v) { return v >= 14 && v <= 28; }),
    Num(Id::SmoothStep,      L"smoothStep",      L"SmoothStep",      120, [](int v) { return v == 70 || v == 120 || v == 200; }),
    Num(Id::SmoothTime,      L"smoothTime",      L"SmoothTime",      400, [](int v) { return v == 250 || v == 400 || v == 700; }),
    Num(Id::DevDock,         L"devDock",         L"DevToolsDock",    0,   [](int v) { return v <= 3; }),

    Txt(Id::Lang,         L"lang",         L"Language",     L"en",
        [](const std::wstring& v) { return v == L"en" || v == L"id"; }, false),
    Txt(Id::ReaderTheme,  L"readerTheme",  L"ReaderTheme",  L"neon",
        [](const std::wstring& v) { return v == L"neon" || v == L"dark" || v == L"sepia" || v == L"light"; }),
    Txt(Id::ReaderFont,   L"readerFont",   L"ReaderFont",   L"sans",
        [](const std::wstring& v) { return v == L"sans" || v == L"serif" || v == L"mono"; }),
    Txt(Id::HomePage,     L"home",         L"HomePage",     L"", HomeOk),
    Txt(Id::DohMode,      L"dohMode",      L"DohMode",      L"automatic",
        [](const std::wstring& v) { return v == L"off" || v == L"automatic" || v == L"secure"; }),
    Txt(Id::DohProvider,  L"dohProvider",  L"DohProvider",  L"cloudflare", DohProviderOk),
    Txt(Id::DohCustom,    L"dohCustom",    L"DohCustom",    L"", DohCustomOk),
    Txt(Id::AccentPreset, L"accentPreset", L"AccentPreset", L"synthwave", PresetOk, false),
    Txt(Id::AccentPink,   L"accentPink",   L"AccentPink",   L"#ff2e97", ColorOk, false),
    Txt(Id::AccentCyan,   L"accentCyan",   L"AccentCyan",   L"#00e5ff", ColorOk, false),
};

const int COUNT = (int)Id::Count;
constexpr bool RowsMatchIds() {
    for (int i = 0; i < (int)Id::Count; i++) if ((int)ROWS[i].id != i) return false;
    return true;
}
static_assert(sizeof(ROWS) / sizeof(ROWS[0]) == (size_t)Id::Count, "one row per setting");
static_assert(RowsMatchIds(), "rows are in the order of Id");

std::atomic<int> g_number[COUNT];   // flags and numbers: read on the IO thread for every request
std::mutex g_textMu;
std::wstring g_text[COUNT];
Storage* g_storage = nullptr;
Setup g_setup;

// A flag or number written the one way Set() accepts: digits only, without leading zeros.
bool ParseNumber(const std::wstring& s, int& out) {
    if (s.empty() || s.size() > 9 || (s.size() > 1 && s[0] == L'0')) return false;
    int v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    out = v;
    return true;
}

// Colours are kept in lower case, whatever was typed.
std::wstring Normalized(const Row& r, const std::wstring& v) {
    if (r.id != Id::AccentPink && r.id != Id::AccentCyan) return v;
    COLORREF c; std::wstring n;
    return ParseHexColor(v, c, n) ? n : v;
}

void Save(const Row& r) {
    if (g_setup.incognito && !r.incognitoSaves) return;
    if (r.kind != Kind::Text) { g_storage->WriteNumber(r.store, (unsigned long)g_number[(int)r.id].load()); return; }
    std::wstring v = Text(r.id);
    if (v.empty()) g_storage->Erase(r.store);
    else g_storage->WriteText(r.store, v);
}

// True when the text differed.
bool Put(Id id, const std::wstring& v) {
    std::lock_guard<std::mutex> lk(g_textMu);
    if (g_text[(int)id] == v) return false;
    g_text[(int)id] = v;
    return true;
}

}  // namespace

bool RegistryStorage::ReadNumber(const wchar_t* name, unsigned long& value) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    value = v;
    return true;
}
bool RegistryStorage::ReadText(const wchar_t* name, std::wstring& value) {
    // of any length: the lists of site exceptions grow with every site added
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size || size > 4 * 1024 * 1024) return false;
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return false;
    s.resize(wcsnlen(s.c_str(), s.size()));
    value = std::move(s);
    return true;
}
void RegistryStorage::WriteNumber(const wchar_t* name, unsigned long value) {
    DWORD v = value;
    RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), name, REG_DWORD, &v, sizeof(v));
}
void RegistryStorage::WriteText(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}
void RegistryStorage::Erase(const wchar_t* name) { RegDeleteKeyValueW(HKEY_CURRENT_USER, key_.c_str(), name); }

void Load(Storage& storage, Setup setup) {
    g_storage = &storage;
    g_setup = std::move(setup);
    for (const Row& r : ROWS) {
        const int i = (int)r.id;
        if (r.kind == Kind::Text) {
            std::wstring v;
            if (!storage.ReadText(r.store, v) || !r.textOk(v)) v = r.text;
            Put(r.id, Normalized(r, v));
            continue;
        }
        unsigned long v = 0;
        const bool stored = storage.ReadNumber(r.store, v);
        int value = r.number;
        if (r.id == Id::ReducedMotion || r.id == Id::HighContrast) {
            const bool system = r.id == Id::ReducedMotion ? g_setup.systemReducedMotion : g_setup.systemHighContrast;
            value = (!stored || v == FOLLOW_SYSTEM) ? (system ? 1 : 0) : (v != 0 ? 1 : 0);
        } else if (stored && r.kind == Kind::Flag) {
            value = v != 0 ? 1 : 0;
        } else if (stored && v <= 0x7fffffffUL && r.numberOk((int)v)) {
            value = (int)v;
        }
        g_number[i] = value;
    }
    // A preset brings its own colours: stored colours count for "custom" only.
    if (const ThemePreset* p = FindPreset(Text(Id::AccentPreset))) {
        Put(Id::AccentPink, p->pink);
        Put(Id::AccentCyan, p->cyan);
    }
}

bool Flag(Id id) { return g_number[(int)id].load() != 0; }
int Number(Id id) { return g_number[(int)id].load(); }
std::wstring Text(Id id) {
    std::lock_guard<std::mutex> lk(g_textMu);
    return g_text[(int)id];
}

void AppendJson(std::string& json) {
    bool first = true;
    for (const Row& r : ROWS) {
        if (!first) json += ',';
        first = false;
        json += JsonUtf8(r.page) + ":";
        if (r.kind == Kind::Flag) json += Flag(r.id) ? "true" : "false";
        else if (r.kind == Kind::Number) json += std::to_string(Number(r.id));
        else json += JsonUtf8(Text(r.id));
    }
}

bool Find(const std::wstring& pageName, Id& id) {
    for (const Row& r : ROWS) if (pageName == r.page) { id = r.id; return true; }
    return false;
}

bool Allowed(Id id, const std::wstring& value) {
    const Row& r = ROWS[(int)id];
    if (r.kind == Kind::Text) return r.textOk(value);
    int v = 0;
    if (!ParseNumber(value, v)) return false;
    return r.kind == Kind::Flag ? v <= 1 : r.numberOk(v);
}

bool Set(Id id, const std::wstring& value) {
    if (!Allowed(id, value)) return false;
    const Row& r = ROWS[(int)id];
    bool changed;
    if (r.kind != Kind::Text) {
        int v = 0;
        ParseNumber(value, v);
        changed = g_number[(int)id].exchange(v) != v;
        Save(r);
    } else {
        changed = Put(id, Normalized(r, value));
        Save(r);
        if (id == Id::AccentPreset) {
            if (const ThemePreset* p = FindPreset(value)) {
                changed |= Put(Id::AccentPink, p->pink);
                changed |= Put(Id::AccentCyan, p->cyan);
                Save(ROWS[(int)Id::AccentPink]);
                Save(ROWS[(int)Id::AccentCyan]);
            }
        } else if (id == Id::AccentPink || id == Id::AccentCyan) {
            changed |= Put(Id::AccentPreset, L"custom");
            Save(ROWS[(int)Id::AccentPreset]);
        }
    }
    if (changed && g_setup.onChange) g_setup.onChange(id);
    return true;
}

bool SetNumber(Id id, int value) { return Set(id, std::to_wstring(value)); }

}  // namespace settings
