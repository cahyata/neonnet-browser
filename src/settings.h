// NeonNet's settings: every single choice the user makes that is remembered (see GLOSSARY.md).
// This module is their only owner. settings.cpp has one table row per setting, which says its
// name on the settings page, its name in storage, its type, its default, the allowed values and
// whether an incognito window may save it.
//
// Reading and Allowed() are safe on any thread. Load() and Set() belong to the UI thread.
#pragma once
#include <functional>
#include <string>

namespace settings {

enum class Id {
    // on / off
    Scanlines, AutoUpdate, AskDownload, FastDownload, DlSubfolder, DlCategories, RestoreSession, Favicons,
    BookmarksBar, HttpsOnly, BlockAds, BlockTrackers, StripTracking, Block3pCookies, StrictReferrer,
    SavePasswords, A11yTree, ReducedMotion, HighContrast, WelcomeDismissed, SmoothScroll, NetSpeed,
    ForceDark, LightMode, BgLimit, BatterySaver, BatteryAuto, NetLimitAll,
    // numbers
    MinFontSize, FastConnections, RamLimitMB, NetLimitKB, DockDefault, DockWidth, ReaderSize, SmoothStep,
    SmoothTime, DevDock,
    // text
    Lang, ReaderTheme, ReaderFont, HomePage, DohMode, DohProvider, DohCustom, AccentPreset, AccentPink, AccentCyan,
    Count
};

// Where values are kept: the registry in NeonNet, memory in the tests.
class Storage {
public:
    virtual ~Storage() = default;
    virtual bool ReadNumber(const wchar_t* name, unsigned long& value) = 0;   // false: not stored
    virtual bool ReadText(const wchar_t* name, std::wstring& value) = 0;
    virtual void WriteNumber(const wchar_t* name, unsigned long value) = 0;
    virtual void WriteText(const wchar_t* name, const std::wstring& value) = 0;
    virtual void Erase(const wchar_t* name) = 0;
};

// Values under HKEY_CURRENT_USER\<key>.
class RegistryStorage : public Storage {
public:
    explicit RegistryStorage(const wchar_t* key) : key_(key) {}
    bool ReadNumber(const wchar_t* name, unsigned long& value) override;
    bool ReadText(const wchar_t* name, std::wstring& value) override;
    void WriteNumber(const wchar_t* name, unsigned long value) override;
    void WriteText(const wchar_t* name, const std::wstring& value) override;
    void Erase(const wchar_t* name) override;
private:
    std::wstring key_;
};

struct Setup {
    bool incognito = false;   // some settings are then kept in memory only
    // What Windows says, for the two settings that follow it until the user chooses.
    bool systemReducedMotion = false, systemHighContrast = false;
    // Called after Set() changed a value, whoever asked for the change. May call Set() again.
    std::function<void(Id)> onChange;
};

// Reads every setting. A stored value that is not allowed counts as not stored.
// Call before anything else; `storage` must live as long as Set() is used.
void Load(Storage& storage, Setup setup);

bool Flag(Id id);
int Number(Id id);
std::wstring Text(Id id);

// Appends "name":value for every setting, comma separated, under the names the settings page uses.
void AppendJson(std::string& json);

bool Find(const std::wstring& pageName, Id& id);
// Values are written as the settings page sends them: "0" / "1", a decimal number, or text.
bool Allowed(Id id, const std::wstring& value);

// The only way to change a setting: checks the value, keeps it, saves it and calls onChange if
// it differs from before. False (and nothing happens) when the value is not allowed.
// Choosing an accent preset also sets both accent colours; setting a colour makes the preset "custom".
bool Set(Id id, const std::wstring& value);
bool SetNumber(Id id, int value);   // also for flags: 0 or 1

}  // namespace settings
