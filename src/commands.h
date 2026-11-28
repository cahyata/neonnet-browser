// Commands and their keyboard shortcuts (see GLOSSARY.md): which commands there are, what a pressed
// key means, and the shortcuts the user changed. What a command does is up to the caller.
//
// Everything is safe on any thread, except Load(), which comes before anything else.
#pragma once
#include <string>
#include <string_view>

namespace commands {

enum class Id {
    None,
    NewTab, CloseTab, ZoomIn, ZoomOut, ZoomReset, Scanlines, Fullscreen, DevTools, About, Reload, Stop, Back, Forward,
    FocusAddress, NextTab, PrevTab, Home, Print,
    UpdateCheck, UpdateAuto, UpdateRestart, UpdatePage,
    Find, FindNext, FindPrev, Reopen, Bookmark, Bookmarks, BookmarksBar, History,
    Downloads, Settings, Incognito, Performance, BlockAds, BlockTrackers, Extensions, Help, Caret,
    Palette, PinTab, MuteTab, ForceDark, DarkExempt, ProxyToggle,
    SplitView, SplitSwap, ReaderMode, SocialDock, LightMode, CopyClean,
    Count
};

// ---- shortcuts

// Where the changed shortcuts are kept.
struct Storage {
    virtual ~Storage() = default;
    virtual bool Read(std::string& data) = 0;   // false when nothing is saved
    virtual void Write(const std::string& data) = 0;
};

class FileStorage : public Storage {
public:
    explicit FileStorage(std::wstring path) : path_(std::move(path)) {}
    bool Read(std::string& data) override;
    void Write(const std::string& data) override;   // all or nothing
private:
    std::wstring path_;
};

// The defaults, then what `storage` holds. `storage` must live as long as shortcuts change. An
// incognito window saves nothing.
void Load(Storage& storage, bool incognito);

// "Ctrl+Shift+T"; empty when the command has no shortcut.
std::wstring Shortcut(Id id);
// For the settings page: every command whose shortcut can be changed, with its present and default keys.
std::string SettingsJson();
// For the toolbar: command name -> keys.
std::wstring KeysJson();
// The command a name stands for, as NeonNet's toolbar sends it ("newtab"). None for an unknown name.
Id FromName(std::wstring_view name);

// Gives the command new keys; empty keys remove its shortcut. False, with the reason in `error`, when
// the keys cannot be used.
bool Rebind(std::string_view name, std::wstring_view keys, std::string& error);
// The default keys again, for one command or (empty name) for all. False, with the reason in `error`,
// when another command holds them.
bool Reset(std::string_view name, std::string& error);

// ---- pressed keys

// Developer tools are a page with shortcuts of its own, so only some commands are taken from them.
enum class Where { Page, DevTools };

struct Action {
    enum Kind {
        None,              // not a shortcut: the key goes on to the page
        Swallow,           // a held key that must not repeat its command: nothing happens
        Run,               // run `id`
        GoToTab,           // tab number `number`, 1-8; 9 is the last tab
        GoToWorkspace,     // workspace number `number`, 1-9
        LeaveFullscreen,   // if the user made the window fullscreen
        CycleFocus,        // move the focus between the toolbar and the page
    };
    Kind kind = None;
    Id id = Id::None;
    int number = 0;
};

// What a key press means. `key` is a Windows virtual key code.
Action OnKey(Where where, bool ctrl, bool shift, bool alt, bool repeat, int key);

}  // namespace commands
