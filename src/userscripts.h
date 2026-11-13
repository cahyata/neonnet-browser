// Userscript manager: loads, parses, saves, and matches .user.js scripts from
// %LOCALAPPDATA%\NeonNet\userscripts\ so users can extend websites without Chrome's extension UI.
#pragma once
#include <string>
#include <vector>

namespace userscripts {

enum RunAt { RUN_END = 0, RUN_START = 1, RUN_IDLE = 2 };

struct Script {
    long long id = 0;
    bool enabled = true;
    std::wstring name = L"Untitled script";
    std::wstring namespaceUri;
    std::wstring version;
    std::wstring author;
    std::wstring description;
    RunAt runAt = RUN_END;
    std::vector<std::wstring> matches;
    std::vector<std::wstring> includes;
    std::vector<std::wstring> excludes;
    std::vector<std::wstring> grants;
    std::string code;   // UTF-8 full .user.js source
};

struct Injected {
    long long id = 0;
    std::wstring label;   // synthetic source label for DevTools stack traces
    std::string js;       // wrapped JS ready for ExecuteJavaScript
};

void Load(const std::wstring& dataDir);
void LoadReadOnly(const std::wstring& dataDir);
bool IsReadOnly();

Script ParseMeta(const std::string& utf8Code);
bool MatchesUrl(const Script& s, const std::wstring& url);

// Returns wrapped scripts that match |url| for the requested |phase| (RUN_START / RUN_END / RUN_IDLE).
std::vector<Injected> ScriptsForUrl(const std::wstring& url, RunAt phase, const wchar_t* appVersion);

// Snapshot counts for Settings summary.
void Counts(size_t& totalOut, size_t& enabledOut);

// Thread-safe JSON APIs for https://neonnet.ui/api/userscripts*
std::string ListJson();
// Parses POST JSON {"id":0,"enabled":true,"code":"..."}. Sets |errOut| and returns 0 on failure.
long long SaveFromJson(const std::string& bodyUtf8, std::string& errOut);
bool SetEnabled(long long id, bool enabled);
bool Delete(long long id);

}  // namespace userscripts
