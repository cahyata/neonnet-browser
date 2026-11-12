// History and bookmarks: kept in memory, saved as UTF-8 text files in the NeonNet data
// folder. Every function is thread-safe (the UI thread records visits, the IO thread
// serves the neonnet.ui pages that list and edit them).
#pragma once
#include <string>
#include <vector>

namespace store {

void Load(const std::wstring& dir);   // dir = %LOCALAPPDATA%\NeonNet
bool SaveIfDirty();                   // returns false if a file could not be written
// Incognito window: reads the bookmarks only. History is not recorded, bookmarks cannot be
// changed, and nothing is written to disk (downloads are listed in memory until the window closes).
void LoadBookmarksReadOnly(const std::wstring& dir);

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

// Distinct http(s) origins of the pages visited since sinceMs (unix ms), newest first, at most max.
std::vector<std::wstring> OriginsSince(long long sinceMs, size_t max);

// Address bar suggestions for the toolbar: JSON [{url, title, bookmark}], best first.
std::string SuggestJson(const std::wstring& query, size_t limit);

// Downloads. The list is saved like the others; a download that was still running when
// NeonNet closed is listed as interrupted next time.
enum DownloadState { DL_ACTIVE = 0, DL_COMPLETE = 1, DL_CANCELED = 2, DL_INTERRUPTED = 3 };
// Creates the entry for cefId on first use, then updates it. Empty strings keep the old value.
// Returns true when the entry is new or its state changed (so the caller should save).
bool DownloadUpdate(unsigned cefId, const std::wstring& url, const std::wstring& path, const std::wstring& name,
                    long long total, long long received, long long speed, int state);
bool DownloadGet(long long id, std::wstring& path, int& state, unsigned& cefId);
bool DownloadOpenable(const std::wstring& path);         // false for programs and scripts
std::string DownloadsJson();                              // newest first
bool DownloadRemove(long long id);                        // not while it is running
void DownloadsClear();                                    // removes everything that is not running

}  // namespace store
