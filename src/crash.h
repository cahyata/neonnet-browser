// Crash report writer: installs an unhandled-exception filter (and abort handler) in the
// browser process that writes %LOCALAPPDATA%\NeonNet\crash.log so failures can be diagnosed
// without a debugger. Loads dbghelp.dll dynamically only when a crash happens.
#pragma once
#include <string>

namespace crash {

void Install(const std::wstring& logDir, const wchar_t* version, const std::wstring& chromium, bool incognito);
void LogSubprocess(const wchar_t* kind, int status, int code);

}  // namespace crash
