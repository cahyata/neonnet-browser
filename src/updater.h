#pragma once
// Self-update helpers: HTTPS download (WinHTTP), GitHub release parsing, SHA-256.
// No CEF dependencies, so this file can also be compiled into small test tools.

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace upd {

struct Release {
    std::wstring tag;           // e.g. "v1.2.0"
    std::wstring version;       // e.g. "1.2.0"
    std::wstring installerUrl;  // .../releases/download/v1.2.0/NeonNet-Setup.exe
    std::wstring shaUrl;        // .../releases/download/v1.2.0/NeonNet-Setup.exe.sha256
    std::wstring pageUrl;       // release page on github.com
};

using Progress = std::function<void(ULONGLONG received, ULONGLONG total)>;

// HTTPS GET. Writes the body into |body| (capped at |maxBody| bytes) or into |file|.
// Returns true only for HTTP 200; |status| receives the HTTP status code (0 on network errors).
bool HttpGet(const std::wstring& url, std::string* body, HANDLE file, size_t maxBody,
             const Progress& progress, const std::atomic<bool>* cancel, DWORD* status);

// Parses the JSON of GET /repos/<repo>/releases/latest. Succeeds only when the release
// carries both NeonNet-Setup.exe and NeonNet-Setup.exe.sha256 assets of this repo.
bool ParseLatest(const std::string& json, const std::wstring& repo, Release& out, std::wstring& err);

// Compares dotted version strings ("1.10.0" > "1.9.3"). Returns <0, 0 or >0.
int CompareVersions(const std::wstring& a, const std::wstring& b);

// Lower-case hex SHA-256 of a file.
bool Sha256File(const std::wstring& path, std::string& hex);

// First 64 hex characters of a "<hash>  <file name>" checksum file, lower-cased.
std::string ParseShaFile(const std::string& text);

}  // namespace upd
