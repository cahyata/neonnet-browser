// History and bookmarks storage (see store.h).
//
// File format, one record per line, UTF-8:  <unix time ms> TAB <url> TAB <title>
// Tabs, newlines and backslashes inside a field are escaped as \t, \n, \r and \\.

#include "store.h"

#include <windows.h>
#include <deque>
#include <vector>
#include <algorithm>

namespace store {
namespace {

struct Entry {
    long long id = 0;
    long long time = 0;   // unix ms
    std::wstring url, title;
};

const size_t MAX_HISTORY = 20000;
const long long HISTORY_DAYS = 90;

SRWLOCK lock = SRWLOCK_INIT;
struct Shared { Shared() { AcquireSRWLockShared(&lock); } ~Shared() { ReleaseSRWLockShared(&lock); } };
struct Excl { Excl() { AcquireSRWLockExclusive(&lock); } ~Excl() { ReleaseSRWLockExclusive(&lock); } };

std::deque<Entry> history;     // oldest first
std::vector<Entry> bookmarks;  // in the order they were added
long long nextId = 1;
bool histDirty = false, bmDirty = false;
std::wstring histFile, bmFile;

long long UnixMs() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return (long long)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000) - 11644473600000LL;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

std::wstring Clean(const std::wstring& s, size_t max) {
    std::wstring o;
    for (wchar_t c : s) o += (c < 0x20) ? L' ' : c;
    if (o.size() > max) o.resize(max);
    return o;
}

std::string Esc(const std::wstring& w) {
    std::string o;
    for (char c : Utf8(w)) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '\t': o += "\\t"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        default: o += c;
        }
    }
    return o;
}

std::wstring Unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else o += s[i];
    }
    return Wide(o);
}

void Json(std::string& o, const std::wstring& w) {
    o += '"';
    for (char c : Utf8(w)) {
        unsigned char u = (unsigned char)c;
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '<') o += "\\u003c";   // keep the JSON safe to inline anywhere
        else if (u < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", u); o += b; }
        else o += c;
    }
    o += '"';
}

void EntryJson(std::string& o, const Entry& e) {
    o += "{\"id\":" + std::to_string(e.id) + ",\"time\":" + std::to_string(e.time) + ",\"url\":";
    Json(o, e.url);
    o += ",\"title\":";
    Json(o, e.title);
    o += '}';
}

template <class C> void ReadFile(const std::wstring& path, C& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::string data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 256LL * 1024 * 1024) {
        data.resize((size_t)size.QuadPart);
        DWORD rd = 0;
        if (!::ReadFile(f, data.data(), (DWORD)data.size(), &rd, nullptr)) rd = 0;
        data.resize(rd);
    }
    CloseHandle(f);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Entry e;
        e.time = _atoi64(line.substr(0, t1).c_str());
        e.url = Unesc(line.substr(t1 + 1, t2 - t1 - 1));
        e.title = Unesc(line.substr(t2 + 1));
        if (e.url.empty() || e.time <= 0) continue;
        e.id = nextId++;
        out.push_back(std::move(e));
    }
}

template <class C> bool WriteFile(const std::wstring& path, const C& items) {
    std::string data;
    for (const Entry& e : items) data += std::to_string(e.time) + '\t' + Esc(e.url) + '\t' + Esc(e.title) + '\n';
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = ::WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size();
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool SafeUrl(const std::wstring& url) {
    std::wstring l = Lower(url);
    return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0 || l.rfind(L"file:///", 0) == 0;
}

}  // namespace

void Load(const std::wstring& dir) {
    Excl x;
    histFile = dir + L"\\history.txt";
    bmFile = dir + L"\\bookmarks.txt";
    history.clear(); bookmarks.clear();
    ReadFile(histFile, history);
    ReadFile(bmFile, bookmarks);
    long long cutoff = UnixMs() - HISTORY_DAYS * 24 * 60 * 60 * 1000;
    size_t before = history.size();
    while (!history.empty() && (history.front().time < cutoff || history.size() > MAX_HISTORY)) history.pop_front();
    histDirty = history.size() != before;
}

bool SaveIfDirty() {
    std::deque<Entry> h; std::vector<Entry> b;
    bool wantH, wantB;
    {
        Excl x;
        wantH = histDirty; wantB = bmDirty;
        if (wantH) h = history;
        if (wantB) b = bookmarks;
        histDirty = bmDirty = false;
    }
    bool ok = true;
    if (wantH && !histFile.empty()) {
        CreateDirectoryW(histFile.substr(0, histFile.find_last_of(L'\\')).c_str(), nullptr);
        if (!WriteFile(histFile, h)) { ok = false; Excl x; histDirty = true; }
    }
    if (wantB && !bmFile.empty()) {
        CreateDirectoryW(bmFile.substr(0, bmFile.find_last_of(L'\\')).c_str(), nullptr);
        if (!WriteFile(bmFile, b)) { ok = false; Excl x; bmDirty = true; }
    }
    return ok;
}

bool Recordable(const std::wstring& url) {
    return SafeUrl(url) && Lower(url).rfind(L"https://neonnet.ui/", 0) != 0 && url.size() < 8192;
}

long long Visit(long long prevId, const std::wstring& url) {
    if (!Recordable(url)) return 0;
    Excl x;
    histDirty = true;
    if (!history.empty() && history.back().url == url && (prevId == history.back().id || prevId == 0)) {
        history.back().time = UnixMs();
        return history.back().id;
    }
    Entry e; e.id = nextId++; e.time = UnixMs(); e.url = url;
    history.push_back(std::move(e));
    while (history.size() > MAX_HISTORY) history.pop_front();
    return history.back().id;
}

void SetTitle(long long id, const std::wstring& title) {
    if (!id) return;
    Excl x;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->id == id) {
            std::wstring t = Clean(title, 500);
            if (it->title != t) { it->title = t; histDirty = true; }
            return;
        }
    }
}

void Forget(long long id, const std::wstring& url) {
    if (!id) return;
    Excl x;
    for (auto it = history.begin(); it != history.end(); ++it)
        if (it->id == id && it->url == url) { history.erase(it); histDirty = true; return; }
}

std::string HistoryJson(const std::wstring& query, size_t limit) {
    std::wstring q = Lower(query);
    std::string o = "[";
    size_t n = 0;
    Shared s;
    for (auto it = history.rbegin(); it != history.rend() && n < limit; ++it) {
        if (!q.empty() && Lower(it->url).find(q) == std::wstring::npos && Lower(it->title).find(q) == std::wstring::npos) continue;
        if (n++) o += ',';
        EntryJson(o, *it);
    }
    return o + "]";
}

bool DeleteHistory(long long id) {
    Excl x;
    for (auto it = history.begin(); it != history.end(); ++it)
        if (it->id == id) { history.erase(it); histDirty = true; return true; }
    return false;
}

void ClearHistory() {
    Excl x;
    history.clear();
    histDirty = true;
}

long long FindBookmark(const std::wstring& url) {
    Shared s;
    for (const Entry& e : bookmarks) if (e.url == url) return e.id;
    return 0;
}

long long AddBookmark(const std::wstring& url, const std::wstring& title) {
    if (!Recordable(url)) return 0;
    Excl x;
    for (const Entry& e : bookmarks) if (e.url == url) return e.id;
    Entry e; e.id = nextId++; e.time = UnixMs(); e.url = url;
    e.title = Clean(title.empty() ? url : title, 500);
    bookmarks.push_back(std::move(e));
    bmDirty = true;
    return bookmarks.back().id;
}

bool DeleteBookmark(long long id) {
    Excl x;
    for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it)
        if (it->id == id) { bookmarks.erase(it); bmDirty = true; return true; }
    return false;
}

bool EditBookmark(long long id, const std::wstring& title, const std::wstring& url) {
    if (!Recordable(url)) return false;
    Excl x;
    for (Entry& e : bookmarks) {
        if (e.id != id) continue;
        e.title = Clean(title.empty() ? url : title, 500);
        e.url = url;
        bmDirty = true;
        return true;
    }
    return false;
}

std::string BookmarksJson() {
    std::string o = "[";
    Shared s;
    for (size_t i = 0; i < bookmarks.size(); i++) {
        if (i) o += ',';
        EntryJson(o, bookmarks[i]);
    }
    return o + "]";
}

}  // namespace store
