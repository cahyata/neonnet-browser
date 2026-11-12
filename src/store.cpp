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
bool histDirty = false, bmDirty = false, dlDirty = false;
std::wstring histFile, bmFile, dlFile;

struct Download {
    long long id = 0, time = 0, total = 0, received = 0, speed = 0;
    unsigned cefId = 0;   // CEF's id for a running download; 0 for entries loaded from disk
    int state = 0;
    std::wstring url, path, name;
};
const size_t MAX_DOWNLOADS = 500;
std::vector<Download> downloads;   // oldest first

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

// downloads.txt: time TAB url TAB name TAB path TAB total TAB state
void ReadDownloads(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::string data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64LL * 1024 * 1024) {
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
        std::vector<std::string> p;
        for (size_t s = 0;;) {
            size_t t = line.find('\t', s);
            p.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        if (p.size() < 6) continue;
        Download d;
        d.time = _atoi64(p[0].c_str());
        d.url = Unesc(p[1]); d.name = Unesc(p[2]); d.path = Unesc(p[3]);
        d.total = _atoi64(p[4].c_str());
        d.state = atoi(p[5].c_str());
        if (d.state == DL_ACTIVE) d.state = DL_INTERRUPTED;
        if (d.state < DL_ACTIVE || d.state > DL_INTERRUPTED || d.time <= 0 || d.name.empty()) continue;
        d.received = d.state == DL_COMPLETE ? d.total : 0;
        d.id = nextId++;
        downloads.push_back(std::move(d));
    }
}

bool WriteDownloads(const std::wstring& path, const std::vector<Download>& items) {
    std::string data;
    for (const Download& d : items)
        data += std::to_string(d.time) + '\t' + Esc(d.url) + '\t' + Esc(d.name) + '\t' + Esc(d.path) + '\t' +
                std::to_string(d.total) + '\t' + std::to_string(d.state) + '\n';
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

std::wstring FileName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

void TrimDownloads() {
    while (downloads.size() > MAX_DOWNLOADS) {
        auto it = std::find_if(downloads.begin(), downloads.end(), [](const Download& d) { return d.state != DL_ACTIVE; });
        if (it == downloads.end()) break;
        downloads.erase(it);
    }
}

}  // namespace

void Load(const std::wstring& dir) {
    Excl x;
    histFile = dir + L"\\history.txt";
    bmFile = dir + L"\\bookmarks.txt";
    dlFile = dir + L"\\downloads.txt";
    history.clear(); bookmarks.clear(); downloads.clear();
    ReadFile(histFile, history);
    ReadFile(bmFile, bookmarks);
    ReadDownloads(dlFile);
    long long cutoff = UnixMs() - HISTORY_DAYS * 24 * 60 * 60 * 1000;
    size_t before = history.size();
    while (!history.empty() && (history.front().time < cutoff || history.size() > MAX_HISTORY)) history.pop_front();
    histDirty = history.size() != before;
}

bool SaveIfDirty() {
    std::deque<Entry> h; std::vector<Entry> b; std::vector<Download> dl;
    bool wantH, wantB, wantD;
    {
        Excl x;
        wantH = histDirty; wantB = bmDirty; wantD = dlDirty;
        if (wantH) h = history;
        if (wantB) b = bookmarks;
        if (wantD) dl = downloads;
        histDirty = bmDirty = dlDirty = false;
    }
    bool ok = true;
    if (wantD && !dlFile.empty()) {
        CreateDirectoryW(dlFile.substr(0, dlFile.find_last_of(L'\\')).c_str(), nullptr);
        if (!WriteDownloads(dlFile, dl)) { ok = false; Excl x; dlDirty = true; }
    }
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

bool DownloadUpdate(unsigned cefId, const std::wstring& url, const std::wstring& path, const std::wstring& name,
                    long long total, long long received, long long speed, int state) {
    if (!cefId) return false;
    Excl x;
    Download* d = nullptr;
    for (auto it = downloads.rbegin(); it != downloads.rend(); ++it)
        if (it->cefId == cefId && it->state == DL_ACTIVE) { d = &*it; break; }
    bool changed = false;
    if (!d) {
        downloads.emplace_back();
        d = &downloads.back();
        d->id = nextId++; d->time = UnixMs(); d->cefId = cefId; d->state = DL_ACTIVE;
        changed = true; dlDirty = true;
        TrimDownloads();
        d = &downloads.back();
    }
    if (!url.empty() && d->url.empty()) d->url = Clean(url, 4096);
    if (!path.empty()) { d->path = path; d->name = Clean(FileName(path), 260); }
    if (!name.empty() && d->name.empty()) d->name = Clean(name, 260);
    if (d->name.empty()) d->name = L"download";
    if (total > 0) d->total = total;
    d->received = received;
    d->speed = speed;
    if (d->state != state) { d->state = state; changed = true; dlDirty = true; }
    if (state != DL_ACTIVE) d->cefId = 0;
    return changed;
}

bool DownloadGet(long long id, std::wstring& path, int& state, unsigned& cefId) {
    Shared s;
    for (const Download& d : downloads) {
        if (d.id != id) continue;
        path = d.path; state = d.state; cefId = d.cefId;
        return true;
    }
    return false;
}

// Programs, scripts and shortcuts are never opened from the downloads page; the user can
// still show them in the folder and decide there.
bool DownloadOpenable(const std::wstring& path) {
    std::wstring name = Lower(FileName(path));
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return true;
    static const wchar_t* bad[] = {
        L".exe", L".msi", L".msp", L".com", L".scr", L".bat", L".cmd", L".ps1", L".psm1", L".vbs", L".vbe", L".js", L".jse",
        L".wsf", L".wsh", L".hta", L".reg", L".lnk", L".url", L".dll", L".jar", L".cpl", L".msc", L".appx", L".msix",
        L".appinstaller", L".application", L".gadget", L".inf", L".pif", L".sys", L".scf", L".ps1xml", L".psc1", L".chm" };
    std::wstring ext = name.substr(dot);
    for (const wchar_t* b : bad) if (ext == b) return false;
    return true;
}

std::string DownloadsJson() {
    std::string o = "[";
    Shared s;
    size_t n = 0;
    for (auto it = downloads.rbegin(); it != downloads.rend() && n < 300; ++it, ++n) {
        const Download& d = *it;
        if (n) o += ',';
        bool exists = d.state == DL_COMPLETE && !d.path.empty() && GetFileAttributesW(d.path.c_str()) != INVALID_FILE_ATTRIBUTES;
        o += "{\"id\":" + std::to_string(d.id) + ",\"time\":" + std::to_string(d.time) + ",\"state\":" + std::to_string(d.state) +
             ",\"total\":" + std::to_string(d.total) + ",\"received\":" + std::to_string(d.received) +
             ",\"speed\":" + std::to_string(d.speed) + ",\"exists\":" + (exists ? "true" : "false") +
             ",\"openable\":" + (exists && DownloadOpenable(d.path) ? "true" : "false") + ",\"name\":";
        Json(o, d.name);
        o += ",\"url\":";
        Json(o, d.url);
        o += '}';
    }
    return o + "]";
}

bool DownloadRemove(long long id) {
    Excl x;
    for (auto it = downloads.begin(); it != downloads.end(); ++it)
        if (it->id == id && it->state != DL_ACTIVE) { downloads.erase(it); dlDirty = true; return true; }
    return false;
}

void DownloadsClear() {
    Excl x;
    downloads.erase(std::remove_if(downloads.begin(), downloads.end(), [](const Download& d) { return d.state != DL_ACTIVE; }), downloads.end());
    dlDirty = true;
}

}  // namespace store
