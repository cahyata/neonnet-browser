// History and bookmarks: kept in memory, saved as UTF-8 text files in the NeonNet data
// folder. Every function is thread-safe (the UI thread records visits, the IO thread
// serves the neonnet.ui pages that list and edit them).
#pragma once
#include <string>

namespace store {

void Load(const std::wstring& dir);   // dir = %LOCALAPPDATA%\NeonNet
bool SaveIfDirty();                   // returns false if a file could not be written

// http, https and file URLs; never NeonNet's own pages.
bool Recordable(const std::wstring& url);

// History. Visit() returns the entry id. When prevId is the newest entry and has the
// same URL (a reload), that entry is refreshed instead of adding a new one.
long long Visit(long long prevId, const std::wstring& url);
void SetTitle(long long id, const std::wstring& title);
void Forget(long long id, const std::wstring& url);       // drop a visit that failed to load
std::string HistoryJson(const std::wstring& query, size_t limit);
bool DeleteHistory(long long id);
void ClearHistory();

// Bookmarks
long long FindBookmark(const std::wstring& url);         // 0 when not bookmarked
long long AddBookmark(const std::wstring& url, const std::wstring& title);
bool DeleteBookmark(long long id);
bool EditBookmark(long long id, const std::wstring& title, const std::wstring& url);
std::string BookmarksJson();

}  // namespace store
