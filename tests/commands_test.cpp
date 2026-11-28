// Tests of the commands module through its interface, with storage in memory.
// Run with: build.ps1 -Test
#include "commands.h"
#include "test.h"
#include <windows.h>
#include <cstdio>

using commands::Action;
using commands::Id;
using commands::Where;

namespace {

struct Saved : commands::Storage {
    std::string data;
    int writes = 0;
    bool Read(std::string& out) override { out = data; return !data.empty(); }
    void Write(const std::string& in) override { data = in; writes++; }
};

Saved store;   // outlives every Load() below

void Fresh(bool incognito = false) {
    store = Saved();
    commands::Load(store, incognito);
}

enum { C = 1, S = 2, A = 4 };
Action Press(int mods, int key, bool repeat = false, Where where = Where::Page) {
    return commands::OnKey(where, (mods & C) != 0, (mods & S) != 0, (mods & A) != 0, repeat, key);
}
bool Runs(const Action& a, Id id) { return a.kind == Action::Run && a.id == id; }

bool Rebind(const char* name, const wchar_t* keys, std::string* why = nullptr) {
    std::string error;
    bool ok = commands::Rebind(name, keys, error);
    if (why) *why = error;
    return ok;
}

struct Default { Id id; int mods; int key; const wchar_t* text; };
const Default kDefaults[] = {
    { Id::NewTab, C, 'T', L"Ctrl+T" }, { Id::CloseTab, C, 'W', L"Ctrl+W" }, { Id::Reopen, C | S, 'T', L"Ctrl+Shift+T" },
    { Id::NextTab, C, VK_TAB, L"Ctrl+Tab" }, { Id::PrevTab, C | S, VK_TAB, L"Ctrl+Shift+Tab" }, { Id::SplitView, C | A, 'S', L"Ctrl+Alt+S" },
    { Id::Palette, C | S, 'P', L"Ctrl+Shift+P" }, { Id::FocusAddress, C, 'L', L"Ctrl+L" }, { Id::Reload, C, 'R', L"Ctrl+R" },
    { Id::Back, A, VK_LEFT, L"Alt+Left" }, { Id::Forward, A, VK_RIGHT, L"Alt+Right" }, { Id::Home, A, VK_HOME, L"Alt+Home" },
    { Id::Find, C, 'F', L"Ctrl+F" }, { Id::FindNext, 0, VK_F3, L"F3" }, { Id::FindPrev, S, VK_F3, L"Shift+F3" },
    { Id::Bookmark, C, 'D', L"Ctrl+D" }, { Id::Bookmarks, C | S, 'O', L"Ctrl+Shift+O" }, { Id::BookmarksBar, C | S, 'B', L"Ctrl+Shift+B" },
    { Id::History, C, 'H', L"Ctrl+H" }, { Id::Downloads, C, 'J', L"Ctrl+J" }, { Id::Extensions, C | S, 'E', L"Ctrl+Shift+E" },
    { Id::Settings, C, VK_OEM_COMMA, L"Ctrl+," }, { Id::Incognito, C | S, 'N', L"Ctrl+Shift+N" }, { Id::ReaderMode, C | A, 'R', L"Ctrl+Alt+R" },
    { Id::SocialDock, C | A, 'W', L"Ctrl+Alt+W" }, { Id::Performance, C | S, 'M', L"Ctrl+Shift+M" }, { Id::DevTools, 0, VK_F12, L"F12" },
    { Id::Print, C, 'P', L"Ctrl+P" }, { Id::Caret, 0, VK_F7, L"F7" }, { Id::Fullscreen, 0, VK_F11, L"F11" },
    { Id::ZoomIn, C, VK_OEM_PLUS, L"Ctrl++" }, { Id::ZoomOut, C, VK_OEM_MINUS, L"Ctrl+-" }, { Id::ZoomReset, C, '0', L"Ctrl+0" },
    { Id::Help, 0, VK_F1, L"F1" },
};

void DefaultShortcuts() {
    Fresh();
    for (const Default& d : kDefaults) {
        CHECK(commands::Shortcut(d.id) == d.text);
        CHECK(Runs(Press(d.mods, d.key), d.id));
    }
    int withKeys = 0;
    for (int i = 1; i < (int)Id::Count; i++) withKeys += !commands::Shortcut((Id)i).empty();
    CHECK(withKeys == (int)(sizeof(kDefaults) / sizeof(kDefaults[0])));
    // the number pad counts as the main keys
    CHECK(Runs(Press(C, VK_ADD), Id::ZoomIn) && Runs(Press(C, VK_SUBTRACT), Id::ZoomOut) && Runs(Press(C, VK_NUMPAD0), Id::ZoomReset));
    // the modifiers must match exactly
    CHECK(Press(C | S, 'W').kind == Action::None && Press(0, 'T').kind == Action::None && Press(C, 'C').kind == Action::None);
}

void HeldKeys() {
    Fresh();
    for (Id id : { Id::NewTab, Id::CloseTab, Id::Reopen, Id::Bookmark, Id::BookmarksBar, Id::Incognito, Id::Performance, Id::History,
                   Id::Downloads, Id::Settings, Id::Bookmarks, Id::Extensions, Id::Palette, Id::Print, Id::SplitView, Id::ReaderMode,
                   Id::SocialDock, Id::DevTools, Id::Caret, Id::Fullscreen, Id::Help })
        for (const Default& d : kDefaults)
            if (d.id == id) CHECK(Press(d.mods, d.key, true).kind == Action::Swallow);
    for (Id id : { Id::NextTab, Id::PrevTab, Id::FocusAddress, Id::Reload, Id::Back, Id::Forward, Id::Home, Id::Find, Id::FindNext,
                   Id::FindPrev, Id::ZoomIn, Id::ZoomOut, Id::ZoomReset })
        for (const Default& d : kDefaults)
            if (d.id == id) CHECK(Runs(Press(d.mods, d.key, true), id));
    // commands without a default shortcut
    const char* once[] = { "pin", "mute", "splitswap", "proxy", "forcedark", "lightmode", "darkexempt", "blockads", "blocktrackers", "scan" };
    for (const char* name : once) {
        CHECK(Rebind(name, L"Ctrl+Alt+G"));
        CHECK(Press(C | A, 'G', true).kind == Action::Swallow);
        CHECK(Rebind(name, L""));
    }
    CHECK(Rebind("copyclean", L"Ctrl+Alt+G"));
    CHECK(Runs(Press(C | A, 'G', true), Id::CopyClean));
}

void DeveloperTools() {
    Fresh();
    int taken = 0;
    for (const Default& d : kDefaults) {
        const Action a = Press(d.mods, d.key, false, Where::DevTools);
        bool expected = false;
        for (Id id : { Id::Incognito, Id::Palette, Id::NewTab, Id::Reopen, Id::History, Id::Downloads, Id::Extensions, Id::Settings,
                       Id::PrevTab, Id::NextTab, Id::Help, Id::Caret, Id::Fullscreen, Id::DevTools })
            expected |= id == d.id;
        CHECK(expected ? Runs(a, d.id) : a.kind == Action::None);
        taken += a.kind == Action::Run;
        CHECK(Press(d.mods, d.key, true, Where::DevTools).kind == Action::None);   // held keys stay with the tools
    }
    CHECK(taken == 14);
    CHECK(Runs(Press(C | S, 'K', false, Where::DevTools), Id::Palette));
    // none of the fixed keys of a page
    CHECK(Press(C, 'G', false, Where::DevTools).kind == Action::None && Press(C, '1', false, Where::DevTools).kind == Action::None);
    CHECK(Press(0, VK_F5, false, Where::DevTools).kind == Action::None && Press(0, VK_F6, false, Where::DevTools).kind == Action::None);
    // a user's shortcut on Ctrl+Shift+K that the tools keep is not turned into the palette
    CHECK(Rebind("print", L"Ctrl+Shift+K"));
    CHECK(Press(C | S, 'K', false, Where::DevTools).kind == Action::None);
}

void FixedKeys() {
    Fresh();
    CHECK(Runs(Press(C, 'G'), Id::FindNext) && Runs(Press(C | S, 'G'), Id::FindPrev) && Runs(Press(C, 'G', true), Id::FindNext));
    CHECK(Runs(Press(C, VK_F4), Id::CloseTab) && Press(C, VK_F4, true).kind == Action::None && Press(C | S, VK_F4).kind == Action::None);
    CHECK(Runs(Press(C | S, 'K'), Id::Palette) && Press(C | S, 'K', true).kind == Action::None);
    CHECK(Runs(Press(0, VK_F5), Id::Reload) && Runs(Press(0, VK_F5, true), Id::Reload) && Press(S, VK_F5).kind == Action::None);
    for (int n = 1; n <= 9; n++) {
        const Action tab = Press(C, '0' + n), repeated = Press(C, '0' + n, true), ws = Press(A, '0' + n);
        CHECK(tab.kind == Action::GoToTab && tab.number == n && repeated.kind == Action::GoToTab);
        CHECK(ws.kind == Action::GoToWorkspace && ws.number == n);
        CHECK(Press(A, '0' + n, true).kind == Action::None && Press(C | S, '0' + n).kind == Action::None && Press(C | A, '0' + n).kind == Action::None);
    }
    CHECK(Press(0, VK_ESCAPE).kind == Action::LeaveFullscreen && Press(S, VK_ESCAPE).kind == Action::None);
    CHECK(Press(0, VK_F6).kind == Action::CycleFocus && Press(S, VK_F6).kind == Action::CycleFocus);
    CHECK(Press(0, VK_F6, true).kind == Action::None && Press(C, VK_F6).kind == Action::None);
    // the user's shortcut comes before a fixed key
    CHECK(Rebind("history", L"Ctrl+1"));
    CHECK(Runs(Press(C, '1'), Id::History));
    CHECK(Rebind("history", L"F5"));
    CHECK(Runs(Press(0, VK_F5), Id::History));
}

void ReadingKeys() {
    Fresh();
    CHECK(Rebind("zoomin", L"") && Rebind("zoomout", L"") && Rebind("settings", L""));   // their keys are tried below
    const struct { const wchar_t* in; const wchar_t* out; } good[] = {
        { L"  ctrl + alt + n ", L"Ctrl+Alt+N" }, { L"Shift+Alt+Ctrl+x", L"Ctrl+Alt+Shift+X" }, { L"Control+plus", L"Ctrl++" },
        { L"Ctrl++", L"Ctrl++" }, { L"Ctrl+=", L"Ctrl++" }, { L"Ctrl+minus", L"Ctrl+-" }, { L"F9", L"F9" }, { L"shift+f10", L"Shift+F10" },
        { L"Alt+ArrowUp", L"Alt+Up" }, { L"Ctrl+PageDown", L"Ctrl+PageDown" }, { L"Ctrl+space", L"Ctrl+Space" }, { L"Ctrl+Enter", L"Ctrl+Enter" },
        { L"Ctrl+\\", L"Ctrl+\\" }, { L"Ctrl+'", L"Ctrl+'" }, { L"Ctrl+comma", L"Ctrl+," }, { L"Ctrl+period", L"Ctrl+." }, { L"Alt+9", L"Alt+9" },
    };
    for (const auto& k : good) {
        CHECK(Rebind("pin", k.in));
        CHECK(commands::Shortcut(Id::PinTab) == k.out);
    }
    for (const wchar_t* bad : { L"A", L"Shift+A", L"Tab", L"Ctrl+F13", L"F0", L"Ctrl+F01", L"Super+A", L"Ctrl+numpad", L"Ctrl+Shift", L"+" }) {
        std::string why;
        CHECK(!Rebind("pin", bad, &why));
        CHECK(why == "Use Ctrl/Alt + key or F1-F12.");
    }
    CHECK(commands::Shortcut(Id::PinTab) == L"Alt+9");   // a refused change leaves the old keys
    CHECK(Rebind("pin", L"  "));
    CHECK(commands::Shortcut(Id::PinTab).empty() && Press(A, '9').kind == Action::GoToWorkspace);
}

void Refusals() {
    Fresh();
    std::string why;
    CHECK(!Rebind("nothing", L"Ctrl+M", &why) && why == "Unknown command.");
    CHECK(!Rebind("about", L"Ctrl+M", &why) && why == "Unknown command.");
    CHECK(!Rebind("", L"Ctrl+M", &why) && why == "Unknown command.");
    for (const wchar_t* k : { L"Ctrl+C", L"ctrl+x", L"Ctrl+V", L"Ctrl+A", L"Ctrl+Z", L"Ctrl+Y" }) CHECK(!Rebind("scan", k));
    CHECK(!Rebind("scan", L"Ctrl+C", &why) && why == "Ctrl+C is kept for web pages (copy, cut, paste, select all, undo, redo).");
    CHECK(Rebind("scan", L"Ctrl+Shift+V") && Rebind("scan", L"Ctrl+Alt+C") && Rebind("scan", L"Ctrl+M"));
    CHECK(!Rebind("mute", L"Ctrl+T", &why) && why == "Shortcut Ctrl+T is already assigned to New tab.");
    CHECK(Rebind("newtab", L"Ctrl+T"));   // its own keys
    CHECK(store.writes == 4);             // refused changes are not saved
}

void Resetting() {
    Fresh();
    std::string why;
    CHECK(Rebind("newtab", L"") && Rebind("closetab", L"Ctrl+T"));
    const int writes = store.writes;
    CHECK(!commands::Reset("newtab", why) && why == "Shortcut Ctrl+T is already assigned to Close tab.");
    CHECK(commands::Shortcut(Id::NewTab).empty() && store.writes == writes);
    CHECK(Rebind("closetab", L""));
    CHECK(commands::Reset("newtab", why) && commands::Shortcut(Id::NewTab) == L"Ctrl+T");
    CHECK(commands::Reset("nothing", why) && commands::Shortcut(Id::CloseTab).empty());
    CHECK(commands::Reset("", why) && commands::Shortcut(Id::CloseTab) == L"Ctrl+W");
}

void Saving() {
    Fresh();
    CHECK(Rebind("newtab", L"Ctrl+Alt+N") && Rebind("closetab", L""));
    CHECK(store.data.rfind("NEONNET-KEYS 1\nnewtab\tCtrl+Alt+N\nclosetab\t\nreopen\tCtrl+Shift+T\n", 0) == 0);
    CHECK(store.data.find("about") == std::string::npos);
    // read back
    Saved again;
    again.data = store.data;
    commands::Load(again, false);
    CHECK(commands::Shortcut(Id::NewTab) == L"Ctrl+Alt+N" && commands::Shortcut(Id::CloseTab).empty() && commands::Shortcut(Id::Find) == L"Ctrl+F");
    // lines that cannot be read keep their default
    again.data = "NEONNET-KEYS 1\r\nnewtab\tCtrl+M\n";
    commands::Load(again, false);
    CHECK(commands::Shortcut(Id::NewTab) == L"Ctrl+T");   // another header
    again.data = "NEONNET-KEYS 1\nnewtab\tnonsense\ngone\tCtrl+M\nno tab here\nfind\tctrl+m\r\nprint\tCtrl+Alt+P";
    commands::Load(again, false);
    CHECK(commands::Shortcut(Id::NewTab) == L"Ctrl+T" && commands::Shortcut(Id::Find) == L"Ctrl+M" && commands::Shortcut(Id::Print) == L"Ctrl+Alt+P");
    // an incognito window changes its shortcuts without saving them
    Fresh(true);
    CHECK(Rebind("newtab", L"Ctrl+Alt+N") && commands::Shortcut(Id::NewTab) == L"Ctrl+Alt+N" && store.writes == 0);
}

void File() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = dir + std::wstring(L"neonnet-keys-test-") + std::to_wstring(GetCurrentProcessId()) + L".dat";
    commands::FileStorage file(path);
    std::string data;
    CHECK(!file.Read(data));
    file.Write("first");
    file.Write("second\n");
    CHECK(file.Read(data) && data == "second\n");
    CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(path.c_str());
}

void Json() {
    Fresh();
    CHECK(Rebind("pin", L"Ctrl+\\"));
    const std::string page = commands::SettingsJson();
    CHECK(page.rfind("{\"incognito\":false,\"items\":[{\"id\":\"newtab\",\"label\":\"New tab\",\"group\":\"Tabs\",\"combo\":\"Ctrl+T\",\"def\":\"Ctrl+T\"},", 0) == 0);
    CHECK(page.find("{\"id\":\"pin\",\"label\":\"Pin / Unpin tab\",\"group\":\"Tabs\",\"combo\":\"Ctrl+\\\\\",\"def\":\"\"}") != std::string::npos);
    CHECK(page.back() == '}' && page.find("about") == std::string::npos);
    const std::wstring keys = commands::KeysJson();
    CHECK(keys.rfind(L"{\"newtab\":\"Ctrl+T\",\"closetab\":\"Ctrl+W\",", 0) == 0);
    CHECK(keys.find(L"\"pin\":\"Ctrl+\\\\\"") != std::wstring::npos && keys.back() == L'}');
    Fresh(true);
    CHECK(commands::SettingsJson().rfind("{\"incognito\":true,", 0) == 0);
}

// Every command the toolbar names must exist.
void ToolbarUsesRealNames() {
    FILE* f = nullptr;
    CHECK(fopen_s(&f, "ui\\chrome.html", "rb") == 0);
    if (!f) return;
    std::string page;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) page.append(buf, n);
    fclose(f);
    Fresh();
    auto names = [&](const std::string& before, bool needsShortcut) {
        int found = 0;
        for (size_t at = page.find(before); at != std::string::npos; at = page.find(before, at + 1)) {
            if (at && (isalnum((unsigned char)page[at - 1]) || page[at - 1] == '_')) continue;   // the end of a longer name
            const size_t start = at + before.size(), end = page.find('\'', start);
            const std::string name = page.substr(start, end - start);
            const Id id = commands::FromName(std::wstring(name.begin(), name.end()));
            std::string why;
            const bool known = id != Id::None && (!needsShortcut || commands::Reset(name, why));
            if (!known || (needsShortcut && commands::KeysJson().find(L"\"" + std::wstring(name.begin(), name.end()) + L"\":") == std::wstring::npos)) {
                printf("unknown command on the toolbar: %s\n", name.c_str());
                CHECK(false);
            }
            found++;
        }
        return found;
    };
    CHECK(names("send('exec', '", false) >= 35);   // buttons and the command palette
    CHECK(names("k('", true) >= 30);               // the shortcut shown beside a palette command
    for (const char* name : { "newtab", "back", "fwd", "wa", "palette", "perf", "downloads", "bookmarks" })
        CHECK(page.find(std::string("', '") + name + "', '") != std::string::npos);   // the tooltips with a shortcut
    CHECK(commands::FromName(L"newtab") == Id::NewTab && commands::FromName(L"about") == Id::About);
    CHECK(commands::FromName(L"") == Id::None && commands::FromName(L"nothing") == Id::None);
}

}  // namespace

void CommandsTests() {
    DefaultShortcuts();
    HeldKeys();
    DeveloperTools();
    FixedKeys();
    ReadingKeys();
    Refusals();
    Resetting();
    Saving();
    File();
    Json();
    ToolbarUsesRealNames();
}
