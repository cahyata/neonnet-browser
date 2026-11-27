// Tests of the settings module through its interface, with storage in memory: nothing on this
// computer is read or changed. Run with: build.ps1 -Test
#include "settings.h"
#include "json.h"
#include <cstdio>
#include <map>
#include <set>
#include <vector>

using settings::Id;

static int checks = 0, failed = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Memory : settings::Storage {
    std::map<std::wstring, unsigned long> numbers;
    std::map<std::wstring, std::wstring> texts;
    int writes = 0;
    bool ReadNumber(const wchar_t* n, unsigned long& v) override {
        auto it = numbers.find(n);
        if (it == numbers.end()) return false;
        v = it->second; return true;
    }
    bool ReadText(const wchar_t* n, std::wstring& v) override {
        auto it = texts.find(n);
        if (it == texts.end()) return false;
        v = it->second; return true;
    }
    void WriteNumber(const wchar_t* n, unsigned long v) override { numbers[n] = v; writes++; }
    void WriteText(const wchar_t* n, const std::wstring& v) override { texts[n] = v; writes++; }
    void Erase(const wchar_t* n) override { numbers.erase(n); texts.erase(n); writes++; }
};

// For every setting: a value that is allowed and differs from the default. A new setting needs a line here.
struct Sample { const wchar_t* name; const wchar_t* other; bool incognitoSaves; };
static const Sample SAMPLES[] = {
    { L"scan", L"0", true }, { L"autoUpdate", L"0", true }, { L"askDownload", L"1", true }, { L"fastDownload", L"0", true },
    { L"dlSubfolder", L"0", true }, { L"dlCategories", L"0", true }, { L"restoreSession", L"1", true }, { L"favicons", L"0", true },
    { L"bookmarksBar", L"0", true }, { L"httpsOnly", L"1", true }, { L"blockAds", L"0", true }, { L"blockTrackers", L"0", true },
    { L"stripTracking", L"0", true }, { L"block3pCookies", L"1", true }, { L"strictReferrer", L"1", true }, { L"savePasswords", L"0", true },
    { L"a11yTree", L"1", true }, { L"reducedMotion", L"1", true }, { L"highContrast", L"1", true }, { L"welcomeDismissed", L"1", false },
    { L"smoothScroll", L"0", true }, { L"netSpeed", L"0", true }, { L"forceDark", L"1", true }, { L"lightMode", L"1", false },
    { L"bgLimit", L"1", true }, { L"batterySaver", L"1", true }, { L"batteryAuto", L"1", true }, { L"netLimitAll", L"1", true },
    { L"minFontSize", L"16", true }, { L"fastConnections", L"16", true }, { L"ramLimitMB", L"2048", true }, { L"netLimitKB", L"512", true },
    { L"dockDefault", L"2", true }, { L"dockWidth", L"640", true }, { L"readerSize", L"22", true }, { L"smoothStep", L"200", true },
    { L"smoothTime", L"700", true }, { L"devDock", L"3", true },
    { L"lang", L"id", false }, { L"readerTheme", L"sepia", true }, { L"readerFont", L"mono", true }, { L"home", L"https://example.com/", true },
    { L"dohMode", L"secure", true }, { L"dohProvider", L"quad9", true }, { L"dohCustom", L"https://dns.example/q", true },
    { L"accentPreset", L"plasma", false }, { L"accentPink", L"#123abc", false }, { L"accentCyan", L"#abcdef", false },
};

static std::wstring Value(Id id) {   // as the settings page would write it
    std::string json;
    settings::AppendJson(json);
    for (const Sample& s : SAMPLES) {
        Id other;
        if (settings::Find(s.name, other) && other == id) {
            std::string key = JsonUtf8(s.name) + ":";
            size_t p = json.find(key);
            if (p == std::string::npos) return L"?";
            p += key.size();
            std::string v = json[p] == '"' ? json.substr(p + 1, json.find('"', p + 1) - p - 1) : json.substr(p, json.find(',', p) - p);
            if (v == "true") v = "1";
            if (v == "false") v = "0";
            return std::wstring(v.begin(), v.end());
        }
    }
    return L"?";
}

static void EverySettingHasASample() {
    std::set<Id> seen;
    for (const Sample& s : SAMPLES) {
        Id id;
        CHECK(settings::Find(s.name, id));
        if (settings::Find(s.name, id)) seen.insert(id);
    }
    CHECK(seen.size() == (size_t)Id::Count);
}

static void EverySettingRoundTrips(bool incognito) {
    for (const Sample& s : SAMPLES) {
        Memory m;
        settings::Setup setup;
        setup.incognito = incognito;
        settings::Load(m, setup);
        Id id;
        if (!settings::Find(s.name, id)) continue;
        std::wstring def = Value(id);
        CHECK(def != s.other);
        CHECK(settings::Set(id, s.other));
        CHECK(Value(id) == s.other);
        settings::Load(m, setup);   // as after a restart
        bool kept = !incognito || s.incognitoSaves;
        CHECK(Value(id) == (kept ? std::wstring(s.other) : def));
        if (!kept) CHECK(m.writes == 0);
    }
}

static void DefaultsWithEmptyStorage() {
    Memory m;
    settings::Load(m, {});
    CHECK(settings::Flag(Id::Scanlines) && settings::Flag(Id::BlockAds) && !settings::Flag(Id::HttpsOnly));
    CHECK(settings::Number(Id::DockWidth) == 500 && settings::Number(Id::SmoothStep) == 120 && settings::Number(Id::FastConnections) == 8);
    CHECK(settings::Text(Id::Lang) == L"en" && settings::Text(Id::HomePage).empty() && settings::Text(Id::DohProvider) == L"cloudflare");
    CHECK(settings::Text(Id::AccentPink) == L"#ff2e97" && settings::Text(Id::AccentCyan) == L"#00e5ff");
    CHECK(m.writes == 0);
}

static void StoredValuesThatAreNotAllowed() {
    Memory m;
    m.numbers = { { L"SmoothStep", 100 }, { L"MinFontSize", 13 }, { L"DockWidth", 5000 }, { L"RamLimitMB", 0xffffffffUL }, { L"BgLimit", 7 } };
    m.texts = { { L"ReaderTheme", L"blue" }, { L"HomePage", L"ftp://x" }, { L"DohCustom", L"http://x" }, { L"AccentPreset", L"nope" }, { L"AccentPink", L"#000000" } };
    settings::Load(m, {});
    CHECK(settings::Number(Id::SmoothStep) == 120);
    CHECK(settings::Number(Id::MinFontSize) == 0);
    CHECK(settings::Number(Id::DockWidth) == 500);
    CHECK(settings::Number(Id::RamLimitMB) == 0);
    CHECK(settings::Flag(Id::BgLimit));   // any non-zero number is on
    CHECK(settings::Text(Id::ReaderTheme) == L"neon");
    CHECK(settings::Text(Id::HomePage).empty() && settings::Text(Id::DohCustom).empty());
    CHECK(settings::Text(Id::AccentPreset) == L"synthwave" && settings::Text(Id::AccentPink) == L"#ff2e97");
}

static void RejectedChangesDoNothing() {
    Memory m;
    int calls = 0;
    settings::Setup setup;
    setup.onChange = [&](Id) { calls++; };
    settings::Load(m, setup);
    struct Bad { Id id; const wchar_t* v; };
    const Bad bad[] = { { Id::HttpsOnly, L"2" }, { Id::HttpsOnly, L"" }, { Id::HttpsOnly, L"x" }, { Id::MinFontSize, L"13" }, { Id::MinFontSize, L"016" },
                        { Id::RamLimitMB, L"abc" }, { Id::RamLimitMB, L"100" }, { Id::DockWidth, L"-300" }, { Id::DockWidth, L"99999999999" },
                        { Id::Lang, L"fr" }, { Id::HomePage, L"javascript:alert(1)" }, { Id::DohCustom, L"https://a b" }, { Id::AccentPink, L"red" } };
    for (const Bad& b : bad) {
        CHECK(!settings::Allowed(b.id, b.v));
        CHECK(!settings::Set(b.id, b.v));
    }
    CHECK(!settings::Allowed(Id::HomePage, L"https://" + std::wstring(500, L'a')));
    CHECK(settings::Allowed(Id::HomePage, L"HTTP://UPPER.example") && settings::Allowed(Id::HomePage, L"file:///C:/x.html"));
    CHECK(calls == 0 && m.writes == 0);
    Id id;
    CHECK(!settings::Find(L"checkUpdate", id) && !settings::Find(L"search", id) && !settings::Find(L"nosuchkey", id));
}

static void ListenerHearsChangesOnly() {
    Memory m;
    std::vector<Id> heard;
    settings::Setup setup;
    setup.onChange = [&](Id id) { heard.push_back(id); };
    settings::Load(m, setup);
    CHECK(settings::SetNumber(Id::HttpsOnly, 0));   // already off: saved, not announced
    CHECK(heard.empty() && m.numbers.count(L"HttpsOnly") == 1);
    CHECK(settings::SetNumber(Id::HttpsOnly, 1));
    CHECK(settings::SetNumber(Id::HttpsOnly, 1));
    CHECK(heard.size() == 1 && heard[0] == Id::HttpsOnly);
}

static void EmptyTextIsErased() {
    Memory m;
    settings::Load(m, {});
    CHECK(settings::Set(Id::HomePage, L"https://example.com/"));
    CHECK(m.texts.count(L"HomePage") == 1);
    CHECK(settings::Set(Id::HomePage, L""));
    CHECK(m.texts.count(L"HomePage") == 0);
}

static void WindowsDecidesUntilTheUserDoes() {
    Memory m;
    settings::Setup setup;
    setup.systemReducedMotion = true;
    settings::Load(m, setup);
    CHECK(settings::Flag(Id::ReducedMotion) && !settings::Flag(Id::HighContrast));
    m.numbers[L"ReducedMotion"] = 2;
    m.numbers[L"HighContrast"] = 1;
    settings::Load(m, setup);
    CHECK(settings::Flag(Id::ReducedMotion) && settings::Flag(Id::HighContrast));
    CHECK(settings::SetNumber(Id::ReducedMotion, 1));   // choosing what Windows says still counts as choosing
    CHECK(m.numbers[L"ReducedMotion"] == 1);
    settings::Load(m, {});
    CHECK(settings::Flag(Id::ReducedMotion));
}

static void AccentRules() {
    Memory m;
    int calls = 0;
    settings::Setup setup;
    setup.onChange = [&](Id) { calls++; };
    settings::Load(m, setup);
    CHECK(settings::Set(Id::AccentPreset, L"plasma"));
    CHECK(settings::Text(Id::AccentPink) == L"#00e5ff" && settings::Text(Id::AccentCyan) == L"#b967ff");
    CHECK(m.texts[L"AccentPink"] == L"#00e5ff" && m.texts[L"AccentCyan"] == L"#b967ff" && calls == 1);
    CHECK(settings::Set(Id::AccentPreset, L"custom"));
    CHECK(settings::Text(Id::AccentPink) == L"#00e5ff" && calls == 2);
    CHECK(settings::Set(Id::AccentPreset, L"amber"));
    CHECK(settings::Set(Id::AccentPink, L"#123ABC"));
    CHECK(settings::Text(Id::AccentPreset) == L"custom" && settings::Text(Id::AccentPink) == L"#123abc");
    CHECK(settings::Text(Id::AccentCyan) == L"#3db8ff" && m.texts[L"AccentPreset"] == L"custom" && calls == 4);
    CHECK(settings::Set(Id::AccentPreset, L"amber"));
    // the same colour again: the preset turns custom, which is a change
    CHECK(settings::Set(Id::AccentPink, L"#ff9a3c"));
    CHECK(settings::Text(Id::AccentPreset) == L"custom" && calls == 6);
    settings::Load(m, setup);
    CHECK(settings::Text(Id::AccentPink) == L"#ff9a3c" && settings::Text(Id::AccentCyan) == L"#3db8ff");
}

static void JsonShape() {
    Memory m;
    settings::Load(m, {});
    std::string json = "{";
    settings::AppendJson(json);
    json += "}";
    CHECK(json.find("{\"scan\":true,") == 0);
    CHECK(json.find("\"dockWidth\":500,") != std::string::npos);
    CHECK(json.find("\"home\":\"\",") != std::string::npos);
    CHECK(JsonUtf8(L"a\"b\\c<d\n\u00e9") == "\"a\\\"b\\\\c\\u003cd\\u000a\xc3\xa9\"");
}

// Every switch the settings page wires up must name a real setting, or it could never be turned off.
static void SettingsPageUsesRealNames() {
    FILE* f = nullptr;
    CHECK(fopen_s(&f, "ui\\settings.html", "rb") == 0);
    if (!f) return;
    std::string page;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) page.append(buf, n);
    fclose(f);
    size_t p = page.find("for(const [id, key] of [['#");
    CHECK(p != std::string::npos);
    if (p == std::string::npos) return;
    std::string list = page.substr(p, page.find("]]){", p) - p + 2);
    int toggles = 0;
    for (size_t c = list.find("','"); c != std::string::npos; c = list.find("','", c + 1)) {
        size_t start = c + 3, end = list.find('\'', start);
        std::string name = list.substr(start, end - start);
        Id id;
        bool known = settings::Find(std::wstring(name.begin(), name.end()), id);
        if (!known) printf("unknown setting on the page: %s\n", name.c_str());
        CHECK(known);
        toggles++;
    }
    CHECK(toggles >= 20);
}

int main() {
    EverySettingHasASample();
    EverySettingRoundTrips(false);
    EverySettingRoundTrips(true);
    DefaultsWithEmptyStorage();
    StoredValuesThatAreNotAllowed();
    RejectedChangesDoNothing();
    ListenerHearsChangesOnly();
    EmptyTextIsErased();
    WindowsDecidesUntilTheUserDoes();
    AccentRules();
    JsonShape();
    SettingsPageUsesRealNames();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
