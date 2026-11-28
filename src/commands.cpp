#include "commands.h"
#include "json.h"
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace commands {
namespace {

constexpr unsigned Once = 1;      // holding the keys down runs the command once, not again and again
constexpr unsigned InTools = 2;   // runs while the developer tools have the focus

struct Command {
    Id id;
    const char* name;      // empty: the toolbar cannot ask for it
    const wchar_t* label;  // null: its shortcut cannot be changed, and it has none
    const wchar_t* group;
    const wchar_t* keys;   // the default shortcut
    unsigned flags;
};

// One row for each command. The settings page lists them in this order, and the first row whose keys
// match a pressed key wins.
constexpr Command kCommands[] = {
    { Id::NewTab,        "newtab",        L"New tab",                 L"Tabs",       L"Ctrl+T",         Once | InTools },
    { Id::CloseTab,      "closetab",      L"Close tab",               L"Tabs",       L"Ctrl+W",         Once },
    { Id::Reopen,        "reopen",        L"Reopen closed tab",       L"Tabs",       L"Ctrl+Shift+T",   Once | InTools },
    { Id::NextTab,       "nexttab",       L"Next tab",                L"Tabs",       L"Ctrl+Tab",       InTools },
    { Id::PrevTab,       "prevtab",       L"Previous tab",            L"Tabs",       L"Ctrl+Shift+Tab", InTools },
    { Id::PinTab,        "pin",           L"Pin / Unpin tab",         L"Tabs",       L"",               Once },
    { Id::MuteTab,       "mute",          L"Mute / Unmute tab",       L"Tabs",       L"",               Once },
    { Id::SplitView,     "split",         L"Toggle Split View",       L"Tabs",       L"Ctrl+Alt+S",     Once },
    { Id::SplitSwap,     "splitswap",     L"Swap Split View Panes",   L"Tabs",       L"",               Once },
    { Id::Palette,       "palette",       L"Command palette",         L"Navigation", L"Ctrl+Shift+P",   Once | InTools },
    { Id::FocusAddress,  "focusurl",      L"Focus address bar",       L"Navigation", L"Ctrl+L",         0 },
    { Id::Reload,        "reload",        L"Reload page",             L"Navigation", L"Ctrl+R",         0 },
    { Id::Back,          "back",          L"Back",                    L"Navigation", L"Alt+Left",       0 },
    { Id::Forward,       "fwd",           L"Forward",                 L"Navigation", L"Alt+Right",      0 },
    { Id::Home,          "home",          L"Home",                    L"Navigation", L"Alt+Home",       0 },
    { Id::Find,          "find",          L"Find in page",            L"Navigation", L"Ctrl+F",         0 },
    { Id::FindNext,      "findnext",      L"Find next",               L"Navigation", L"F3",             0 },
    { Id::FindPrev,      "findprev",      L"Find previous",           L"Navigation", L"Shift+F3",       0 },
    { Id::Bookmark,      "bookmark",      L"Bookmark this page",      L"Pages",      L"Ctrl+D",         Once },
    { Id::Bookmarks,     "bookmarks",     L"Bookmarks manager",       L"Pages",      L"Ctrl+Shift+O",   Once },
    { Id::BookmarksBar,  "bookmarks_bar", L"Toggle bookmarks bar",    L"Pages",      L"Ctrl+Shift+B",   Once },
    { Id::History,       "history",       L"History",                 L"Pages",      L"Ctrl+H",         Once | InTools },
    { Id::Downloads,     "downloads",     L"Downloads",               L"Pages",      L"Ctrl+J",         Once | InTools },
    { Id::Extensions,    "extensions",    L"Extensions (Userscripts)", L"Pages",     L"Ctrl+Shift+E",   Once | InTools },
    { Id::Settings,      "settings",      L"Settings",                L"Pages",      L"Ctrl+,",         Once | InTools },
    { Id::Incognito,     "incognito",     L"New incognito window",    L"Windows",    L"Ctrl+Shift+N",   Once | InTools },
    { Id::ProxyToggle,   "proxy",         L"Toggle Proxy Tunnel",     L"Features",   L"",               Once },
    { Id::ReaderMode,    "reader",        L"Toggle Reader Mode",      L"Features",   L"Ctrl+Alt+R",     Once },
    { Id::SocialDock,    "wa",            L"Toggle Social Dock",      L"Features",   L"Ctrl+Alt+W",     Once },
    { Id::ForceDark,     "forcedark",     L"Toggle Force Dark Mode",  L"Features",   L"",               Once },
    { Id::LightMode,     "lightmode",     L"Toggle Light / Dark UI",  L"Features",   L"",               Once },
    { Id::DarkExempt,    "darkexempt",    L"Exclude Site from Force Dark", L"Features", L"",            Once },
    { Id::BlockAds,      "blockads",      L"Toggle Ad Blocker",       L"Features",   L"",               Once },
    { Id::BlockTrackers, "blocktrackers", L"Toggle Tracker Blocker",  L"Features",   L"",               Once },
    { Id::CopyClean,     "copyclean",     L"Copy clean page URL",     L"Features",   L"",               0 },
    { Id::Scanlines,     "scan",          L"Toggle Scanlines",        L"Features",   L"",               Once },
    { Id::Performance,   "perf",          L"Performance panel",       L"Tools",      L"Ctrl+Shift+M",   Once },
    { Id::DevTools,      "devtools",      L"Developer tools",         L"Tools",      L"F12",            Once | InTools },
    { Id::Print,         "print",         L"Print page",              L"Tools",      L"Ctrl+P",         Once },
    { Id::Caret,         "caret",         L"Caret browsing",          L"Tools",      L"F7",             Once | InTools },
    { Id::Fullscreen,    "fullscreen",    L"Toggle fullscreen",       L"Window",     L"F11",            Once | InTools },
    { Id::ZoomIn,        "zoomin",        L"Zoom in",                 L"Zoom",       L"Ctrl++",         0 },
    { Id::ZoomOut,       "zoomout",       L"Zoom out",                L"Zoom",       L"Ctrl+-",         0 },
    { Id::ZoomReset,     "zoomreset",     L"Reset zoom",              L"Zoom",       L"Ctrl+0",         0 },
    { Id::Help,          "help",          L"User guide",              L"Tools",      L"F1",             Once | InTools },
    // without a shortcut
    { Id::Stop,          "",              nullptr, nullptr, L"", 0 },
    { Id::About,         "about",         nullptr, nullptr, L"", 0 },
    { Id::UpdateCheck,   "update_check",  nullptr, nullptr, L"", 0 },
    { Id::UpdateAuto,    "",              nullptr, nullptr, L"", 0 },
    { Id::UpdateRestart, "",              nullptr, nullptr, L"", 0 },
    { Id::UpdatePage,    "",              nullptr, nullptr, L"", 0 },
};
constexpr int kCount = (int)std::size(kCommands);

constexpr bool OneRowEach() {
    for (int id = 1; id < (int)Id::Count; id++) {
        int rows = 0;
        for (const Command& c : kCommands) rows += c.id == (Id)id;
        if (rows != 1) return false;
    }
    return kCount == (int)Id::Count - 1;
}
static_assert(OneRowEach(), "every command needs exactly one row in kCommands");

// ---- key combinations as text

struct Keys {
    bool valid = false;
    bool ctrl = false, shift = false, alt = false;
    int vk = 0;             // 0 with `valid`: no shortcut
    std::wstring text;      // the usual way to write it
};

// The number pad types the same characters as the main keys.
int CanonicalVk(int vk) {
    if (vk == VK_ADD) return VK_OEM_PLUS;
    if (vk == VK_SUBTRACT) return VK_OEM_MINUS;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
    return vk;
}

struct NamedKey { const wchar_t* name; int vk; };
// The first name of a key is the one it is shown with.
constexpr NamedKey kNamed[] = {
    { L"Tab", VK_TAB }, { L"Left", VK_LEFT }, { L"ArrowLeft", VK_LEFT }, { L"Right", VK_RIGHT }, { L"ArrowRight", VK_RIGHT },
    { L"Up", VK_UP }, { L"ArrowUp", VK_UP }, { L"Down", VK_DOWN }, { L"ArrowDown", VK_DOWN }, { L"Home", VK_HOME }, { L"End", VK_END },
    { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Space", VK_SPACE },
    { L"Enter", VK_RETURN }, { L"Backspace", VK_BACK },
    { L",", VK_OEM_COMMA }, { L"comma", VK_OEM_COMMA }, { L".", VK_OEM_PERIOD }, { L"period", VK_OEM_PERIOD },
    { L"+", VK_OEM_PLUS }, { L"=", VK_OEM_PLUS }, { L"plus", VK_OEM_PLUS }, { L"equal", VK_OEM_PLUS },
    { L"-", VK_OEM_MINUS }, { L"minus", VK_OEM_MINUS }, { L"/", VK_OEM_2 }, { L";", VK_OEM_1 }, { L"'", VK_OEM_7 },
    { L"[", VK_OEM_4 }, { L"]", VK_OEM_6 }, { L"\\", VK_OEM_5 }, { L"`", VK_OEM_3 },
};

std::wstring VkName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    for (const NamedKey& k : kNamed) if (k.vk == vk) return k.name;
    return L"";
}

std::wstring Trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

int KeyOf(const std::wstring& lower) {
    if (lower.size() == 1) {
        wchar_t c = lower[0];
        if (c >= L'a' && c <= L'z') return 'A' + (c - L'a');
        if (c >= L'0' && c <= L'9') return c;
    } else if (lower.size() >= 2 && lower[0] == L'f') {
        int n = _wtoi(lower.c_str() + 1);
        if (n >= 1 && n <= 12 && std::to_wstring(n) == lower.substr(1)) return VK_F1 + n - 1;
    }
    for (const NamedKey& k : kNamed) if (lower == Lower(k.name)) return k.vk;
    return 0;
}

// "Ctrl+Shift+T". A shortcut needs Ctrl or Alt unless its key is F1-F12. Empty text means none.
Keys Parse(std::wstring_view raw) {
    Keys out;
    const std::wstring s = Trimmed(std::wstring(raw));
    if (s.empty()) { out.valid = true; return out; }

    // "+" separates the parts and is also a key: "Ctrl++"
    std::vector<std::wstring> parts;
    for (size_t pos = 0; pos < s.size();) {
        size_t plus = s.find(L'+', pos);
        if (plus == std::wstring::npos) { parts.push_back(s.substr(pos)); break; }
        if (plus == pos && plus + 1 == s.size()) { parts.push_back(L"+"); break; }
        parts.push_back(s.substr(pos, plus - pos));
        pos = plus + 1;
        if (pos == s.size()) parts.push_back(L"+");
    }
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        const std::wstring m = Lower(Trimmed(parts[i]));
        if (m == L"ctrl" || m == L"control") out.ctrl = true;
        else if (m == L"alt") out.alt = true;
        else if (m == L"shift") out.shift = true;
        else return Keys{};
    }
    const int vk = KeyOf(Lower(Trimmed(parts.back())));
    if (!vk || (!out.ctrl && !out.alt && !(vk >= VK_F1 && vk <= VK_F12))) return Keys{};

    out.vk = vk;
    out.valid = true;
    if (out.ctrl) out.text += L"Ctrl+";
    if (out.alt) out.text += L"Alt+";
    if (out.shift) out.text += L"Shift+";
    out.text += VkName(vk);
    return out;
}

std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---- the present shortcuts

const char kHeader[] = "NEONNET-KEYS 1";

std::mutex g_mu;
Keys g_keys[kCount];             // the shortcut of kCommands[i]
Storage* g_storage = nullptr;    // null until Load()
bool g_incognito = false;

// The row of a command whose shortcut can be changed, or -1.
int Row(std::string_view name) {
    for (int i = 0; i < kCount; i++)
        if (kCommands[i].label && name == kCommands[i].name) return i;
    return -1;
}

void DefaultsLocked() {
    for (int i = 0; i < kCount; i++) g_keys[i] = Parse(kCommands[i].keys);
}

void SaveLocked() {
    if (!g_storage || g_incognito) return;
    std::string out = std::string(kHeader) + "\n";
    for (int i = 0; i < kCount; i++)
        if (kCommands[i].label) out += std::string(kCommands[i].name) + "\t" + Utf8(g_keys[i].text) + "\n";
    g_storage->Write(out);
}

// Whether another command holds these keys, with the reason to show if so. A combination has one
// owner: of two commands on the same keys, only the first would ever run.
bool TakenLocked(const Keys& keys, int row, std::string& error) {
    if (!keys.vk) return false;
    for (int i = 0; i < kCount; i++) {
        const Keys& k = g_keys[i];
        if (i != row && k.vk == keys.vk && k.ctrl == keys.ctrl && k.shift == keys.shift && k.alt == keys.alt) {
            error = "Shortcut " + Utf8(keys.text) + " is already assigned to " + Utf8(kCommands[i].label) + ".";
            return true;
        }
    }
    return false;
}

// The command whose shortcut this is, or null.
const Command* Match(bool ctrl, bool shift, bool alt, int vk) {
    const int key = CanonicalVk(vk);
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < kCount; i++) {
        const Keys& k = g_keys[i];
        if (k.vk && k.ctrl == ctrl && k.shift == shift && k.alt == alt && k.vk == key) return &kCommands[i];
    }
    return nullptr;
}

Action Run(Id id) { return { Action::Run, id, 0 }; }

}  // namespace

bool FileStorage::Read(std::string& data) {
    HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    bool ok = size > 0 && size < 256 * 1024;
    if (ok) {
        data.resize(size);
        ok = ReadFile(h, data.data(), size, &got, nullptr) && got == size;
    }
    CloseHandle(h);
    return ok;
}

void FileStorage::Write(const std::string& data) {
    const std::wstring tmp = path_ + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

void Load(Storage& storage, bool incognito) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_storage = &storage;
    g_incognito = incognito;
    DefaultsLocked();
    std::string data;
    if (!storage.Read(data)) return;
    // a header line, then "name<tab>keys" lines; what cannot be read keeps its default
    size_t pos = data.find('\n');
    if (pos == std::string::npos || data.compare(0, pos, kHeader) != 0) return;
    for (++pos; pos < data.size();) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        const std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const int row = Row(std::string_view(line).substr(0, tab));
        if (row < 0) continue;
        Keys keys = Parse(Wide(std::string_view(line).substr(tab + 1)));
        if (keys.valid) g_keys[row] = std::move(keys);
    }
}

std::wstring Shortcut(Id id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < kCount; i++)
        if (kCommands[i].id == id) return g_keys[i].text;
    return L"";
}

std::string SettingsJson() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::string o = std::string("{\"incognito\":") + (g_incognito ? "true" : "false") + ",\"items\":[";
    bool first = true;
    for (int i = 0; i < kCount; i++) {
        const Command& c = kCommands[i];
        if (!c.label) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"id\":" + JsonUtf8(Wide(c.name)) + ",\"label\":" + JsonUtf8(c.label) + ",\"group\":" + JsonUtf8(c.group) +
             ",\"combo\":" + JsonUtf8(g_keys[i].text) + ",\"def\":" + JsonUtf8(c.keys) + "}";
    }
    return o + "]}";
}

std::wstring KeysJson() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::string o = "{";
    for (int i = 0; i < kCount; i++) {
        if (!kCommands[i].label) continue;
        if (o.size() > 1) o += ",";
        o += "\"" + std::string(kCommands[i].name) + "\":" + JsonUtf8(g_keys[i].text);
    }
    return Wide(o + "}");
}

Id FromName(std::wstring_view name) {
    const std::string wanted = Utf8(name);
    if (wanted.empty()) return Id::None;
    for (const Command& c : kCommands)
        if (wanted == c.name) return c.id;
    return Id::None;
}

bool Rebind(std::string_view name, std::wstring_view keys, std::string& error) {
    const int row = Row(name);
    if (row < 0) { error = "Unknown command."; return false; }
    Keys k = Parse(keys);
    if (!k.valid) { error = "Use Ctrl/Alt + key or F1-F12."; return false; }
    // a command on these would take copy, cut, paste, select all, undo or redo away from every page
    if (k.ctrl && !k.alt && !k.shift && std::wstring_view(L"CXVAZY").find((wchar_t)k.vk) != std::wstring_view::npos) {
        error = Utf8(k.text) + " is kept for web pages (copy, cut, paste, select all, undo, redo).";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (TakenLocked(k, row, error)) return false;
    g_keys[row] = std::move(k);
    SaveLocked();
    return true;
}

bool Reset(std::string_view name, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (name.empty()) {
        DefaultsLocked();
    } else if (const int row = Row(name); row >= 0) {
        Keys k = Parse(kCommands[row].keys);
        if (TakenLocked(k, row, error)) return false;
        g_keys[row] = std::move(k);
    }
    SaveLocked();
    return true;
}

Action OnKey(Where where, bool ctrl, bool shift, bool alt, bool repeat, int key) {
    const Command* user = Match(ctrl, shift, alt, key);
    const bool paletteKey = ctrl && shift && !alt && key == 'K';   // a second way to the command palette

    if (where == Where::DevTools) {
        if (repeat) return {};
        if (user) return (user->flags & InTools) ? Run(user->id) : Action{};
        return paletteKey ? Run(Id::Palette) : Action{};
    }

    if (user) {
        if (repeat && (user->flags & Once)) return { Action::Swallow, user->id, 0 };
        return Run(user->id);
    }

    // keys that cannot be changed; a shortcut of the user's on the same keys comes first
    const bool digit = key >= '1' && key <= '9';
    if (ctrl && !alt) {
        if (key == 'G') return Run(shift ? Id::FindPrev : Id::FindNext);
        if (key == VK_F4 && !shift && !repeat) return Run(Id::CloseTab);
        if (paletteKey && !repeat) return Run(Id::Palette);
        if (digit && !shift) return { Action::GoToTab, Id::None, key - '0' };
    } else if (alt && !ctrl && !shift && digit && !repeat) {
        return { Action::GoToWorkspace, Id::None, key - '0' };
    } else if (!ctrl && !alt && !shift) {
        if (key == VK_F5) return Run(Id::Reload);
        if (key == VK_ESCAPE) return { Action::LeaveFullscreen, Id::None, 0 };
    }
    if (key == VK_F6 && !ctrl && !alt && !repeat) return { Action::CycleFocus, Id::None, 0 };
    return {};
}

}  // namespace commands
