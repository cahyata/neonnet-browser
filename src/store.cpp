// History and bookmarks storage (see store.h).
//
// File format, one record per line, UTF-8:  <unix time ms> TAB <url> TAB <title>
// Tabs, newlines and backslashes inside a field are escaped as \t, \n, \r and \\.

#include "store.h"

#include <windows.h>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace store {
namespace {

struct Entry {
    long long id = 0;
    long long time = 0;   // unix ms
    bool folder = false;  // bookmarks only: saved on disk as url = "folder://"
    std::wstring url, title, parent;
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
bool incognito = false;        // nothing is recorded or saved (see LoadBookmarksReadOnly)
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
    o += "{\"id\":" + std::to_string(e.id) + ",\"time\":" + std::to_string(e.time) +
         ",\"folder\":" + (e.folder ? "true" : "false") + ",\"url\":";
    Json(o, e.url);
    o += ",\"title\":";
    Json(o, e.title);
    o += ",\"parent\":";
    Json(o, e.parent);
    o += '}';
}

std::wstring TrimW(std::wstring s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) a++;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) b--;
    return s.substr(a, b - a);
}

std::wstring CleanFolderName(const std::wstring& raw) {
    std::wstring s = Clean(raw, 120);
    for (wchar_t& c : s) if (c == L'/' || c == L'\\') c = L'-';
    return TrimW(std::move(s));
}

std::wstring CleanParentPath(const std::wstring& raw) {
    std::wstring out;
    size_t i = 0;
    while (i < raw.size()) {
        size_t sl = raw.find(L'/', i);
        if (sl == std::wstring::npos) sl = raw.size();
        std::wstring seg = CleanFolderName(raw.substr(i, sl - i));
        if (!seg.empty()) {
            if (!out.empty()) out += L'/';
            out += seg;
        }
        i = sl + 1;
    }
    return out;
}

std::wstring JoinPath(const std::wstring& parent, const std::wstring& name) {
    return parent.empty() ? name : parent + L'/' + name;
}

bool FolderExistsLocked(const std::wstring& parent, const std::wstring& name, long long ignoreId = 0) {
    std::wstring low = Lower(name);
    for (const Entry& e : bookmarks)
        if (e.folder && e.id != ignoreId && Lower(e.parent) == Lower(parent) && Lower(e.title) == low) return true;
    return false;
}

int EnsureFolderLocked(const std::wstring& path, long long tm) {
    std::wstring clean = CleanParentPath(path);
    if (clean.empty()) return 0;
    int added = 0;
    std::wstring cur;
    size_t i = 0;
    while (i < clean.size()) {
        size_t sl = clean.find(L'/', i);
        if (sl == std::wstring::npos) sl = clean.size();
        std::wstring seg = clean.substr(i, sl - i);
        if (!seg.empty()) {
            if (!FolderExistsLocked(cur, seg)) {
                Entry f;
                f.id = nextId++;
                f.time = tm > 0 ? tm : UnixMs();
                f.folder = true;
                f.title = seg;
                f.parent = cur;
                bookmarks.push_back(std::move(f));
                added++;
            }
            cur = JoinPath(cur, seg);
        }
        i = sl + 1;
    }
    return added;
}

void EnsureAllFoldersLocked() {
    std::vector<std::pair<std::wstring, long long>> need;
    for (const Entry& e : bookmarks) if (!e.parent.empty()) need.emplace_back(e.parent, e.time);
    for (const auto& p : need) EnsureFolderLocked(p.first, p.second);
}

std::string HtmlEsc(const std::wstring& w) {
    std::string o;
    for (char c : Utf8(w)) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

void AppendCodePoint(std::wstring& out, unsigned cp) {
    if (cp == 0 || cp > 0x10FFFF) return;
    if (cp <= 0xFFFF) { out += (wchar_t)cp; return; }
    cp -= 0x10000;
    out += (wchar_t)(0xD800 + (cp >> 10));
    out += (wchar_t)(0xDC00 + (cp & 0x3FF));
}

std::wstring HtmlUnesc(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { out += s[i]; continue; }
        size_t semi = s.find(L';', i + 1);
        if (semi == std::wstring::npos || semi - i > 12) { out += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        std::wstring low = Lower(ent);
        if (low == L"amp") out += L'&';
        else if (low == L"lt") out += L'<';
        else if (low == L"gt") out += L'>';
        else if (low == L"quot") out += L'"';
        else if (low == L"apos") out += L'\'';
        else if (low == L"nbsp") out += L' ';
        else if (!ent.empty() && ent[0] == L'#') {
            unsigned cp = 0;
            if (ent.size() > 2 && (ent[1] == L'x' || ent[1] == L'X')) cp = (unsigned)wcstoul(ent.c_str() + 2, nullptr, 16);
            else cp = (unsigned)wcstoul(ent.c_str() + 1, nullptr, 10);
            AppendCodePoint(out, cp);
        } else {
            out += s[i];
            continue;
        }
        i = semi;
    }
    return out;
}

std::wstring DecodeHtmlBytes(const std::string& raw) {
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        std::wstring w((raw.size() - 2) / 2, L'\0');
        memcpy(w.data(), raw.data() + 2, w.size() * 2);
        return w;
    }
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        size_t n = (raw.size() - 2) / 2;
        std::wstring w(n, L'\0');
        for (size_t i = 0; i < n; i++) w[i] = ((unsigned char)raw[2 + i * 2] << 8) | (unsigned char)raw[3 + i * 2];
        return w;
    }
    size_t off = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) ? 3 : 0;
    return Wide(raw.substr(off));
}

std::wstring HtmlAttr(const std::wstring& tag, const wchar_t* name) {
    std::wstring low = Lower(tag);
    std::wstring key = Lower(name) + L"=";
    size_t pos = 0;
    while (pos < low.size()) {
        size_t p = low.find(key, pos);
        if (p == std::wstring::npos) return {};
        if (p > 0 && iswalnum(low[p - 1])) { pos = p + 1; continue; }
        size_t v = p + key.size();
        while (v < tag.size() && iswspace(tag[v])) v++;
        if (v >= tag.size()) return {};
        if (tag[v] == L'"' || tag[v] == L'\'') {
            wchar_t q = tag[v++];
            size_t e = tag.find(q, v);
            if (e == std::wstring::npos) return {};
            return HtmlUnesc(tag.substr(v, e - v));
        }
        size_t e = v;
        while (e < tag.size() && !iswspace(tag[e]) && tag[e] != L'>') e++;
        return HtmlUnesc(tag.substr(v, e - v));
    }
    return {};
}

std::wstring StripTags(const std::wstring& html) {
    std::wstring out;
    bool inTag = false;
    for (wchar_t c : html) {
        if (c == L'<') inTag = true;
        else if (c == L'>') inTag = false;
        else if (!inTag) out += c;
    }
    return TrimW(HtmlUnesc(out));
}

template <class C> void ReadFile(const std::wstring& path, C& out, bool isBookmarks = false) {
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
        size_t t3 = isBookmarks ? line.find('\t', t2 + 1) : std::string::npos;
        Entry e;
        e.time = _atoi64(line.substr(0, t1).c_str());
        e.url = Unesc(line.substr(t1 + 1, t2 - t1 - 1));
        e.title = Unesc(t3 == std::string::npos ? line.substr(t2 + 1) : line.substr(t2 + 1, t3 - t2 - 1));
        if (t3 != std::string::npos) e.parent = CleanParentPath(Unesc(line.substr(t3 + 1)));
        if (isBookmarks && e.url == L"folder://") {
            e.folder = true;
            e.url.clear();
            e.title = CleanFolderName(e.title);
            if (e.title.empty() || e.time <= 0) continue;
        } else if (e.url.empty() || e.time <= 0) continue;
        e.id = nextId++;
        out.push_back(std::move(e));
    }
}

template <class C> bool WriteFile(const std::wstring& path, const C& items) {
    std::string data;
    for (const Entry& e : items) {
        data += std::to_string(e.time) + '\t' + (e.folder ? "folder://" : Esc(e.url)) + '\t' + Esc(e.title);
        if (!e.parent.empty()) data += '\t' + Esc(e.parent);
        data += '\n';
    }
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

// Case-insensitive search; needleLower must already be lower case.
bool ContainsI(const std::wstring& hay, const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    if (hay.size() < needleLower.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= hay.size(); i++) {
        size_t j = 0;
        while (j < needleLower.size() && (wchar_t)towlower(hay[i + j]) == needleLower[j]) j++;
        if (j == needleLower.size()) return true;
    }
    return false;
}

// Lower case, without the scheme, a leading "www." and trailing slashes: "github.com/user".
std::wstring Bare(const std::wstring& url) {
    std::wstring s = Lower(url);
    size_t p = s.find(L"://");
    if (p != std::wstring::npos && p < 12) s.erase(0, p + 3);
    if (s.rfind(L"www.", 0) == 0) s.erase(0, 4);
    while (!s.empty() && s.back() == L'/') s.pop_back();
    return s;
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
    ReadFile(bmFile, bookmarks, true);
    EnsureAllFoldersLocked();
    ReadDownloads(dlFile);
    long long cutoff = UnixMs() - HISTORY_DAYS * 24 * 60 * 60 * 1000;
    size_t before = history.size();
    while (!history.empty() && (history.front().time < cutoff || history.size() > MAX_HISTORY)) history.pop_front();
    histDirty = history.size() != before;
}

// Incognito: the bookmarks of the normal profile can be read (so they still show up in the
// address bar and on the new tab page), but nothing is ever written, and no history is kept.
void LoadBookmarksReadOnly(const std::wstring& dir) {
    Excl x;
    incognito = true;
    histFile.clear(); bmFile.clear(); dlFile.clear();   // SaveIfDirty() writes nothing without a file name
    history.clear(); bookmarks.clear(); downloads.clear();
    ReadFile(dir + L"\\bookmarks.txt", bookmarks, true);
    EnsureAllFoldersLocked();
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
    if (incognito || !Recordable(url)) return 0;
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

// Delete the history entries from sinceMs on (0 = all of it). Returns how many were removed.
int ClearHistorySince(long long sinceMs) {
    Excl x;
    size_t before = history.size();
    history.erase(std::remove_if(history.begin(), history.end(), [&](const Entry& e) { return !sinceMs || e.time >= sinceMs; }), history.end());
    int removed = (int)(before - history.size());
    if (removed) histDirty = true;
    return removed;
}

// Distinct http(s) origins (scheme://host[:port]) of the pages visited since sinceMs, newest first.
std::vector<std::wstring> OriginsSince(long long sinceMs, size_t max) {
    Shared s;
    std::vector<std::wstring> out;
    for (auto it = history.rbegin(); it != history.rend() && out.size() < max; ++it) {
        if (it->time < sinceMs) break;   // oldest first, so everything before this is older too
        const std::wstring& u = it->url;
        if (u.rfind(L"http://", 0) != 0 && u.rfind(L"https://", 0) != 0) continue;
        size_t slash = u.find(L'/', u.find(L"://") + 3);
        std::wstring origin = slash == std::wstring::npos ? u : u.substr(0, slash);
        if (std::find(out.begin(), out.end(), origin) == out.end()) out.push_back(std::move(origin));
    }
    return out;
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
    for (const Entry& e : bookmarks) if (!e.folder && e.url == url) return e.id;
    return 0;
}

long long AddBookmark(const std::wstring& url, const std::wstring& title, const std::wstring& parent) {
    if (incognito || !Recordable(url)) return 0;
    std::wstring p = CleanParentPath(parent);
    Excl x;
    if (p.empty()) {
        for (const Entry& e : bookmarks) if (!e.folder && e.url == url) return e.id;
    } else {
        for (const Entry& e : bookmarks) if (!e.folder && e.url == url && Lower(e.parent) == Lower(p)) return e.id;
    }
    EnsureFolderLocked(p, UnixMs());
    Entry e; e.id = nextId++; e.time = UnixMs(); e.url = url;
    e.title = Clean(title.empty() ? url : title, 500);
    e.parent = p;
    bookmarks.push_back(std::move(e));
    bmDirty = true;
    return bookmarks.back().id;
}

long long AddFolder(const std::wstring& name, const std::wstring& parent) {
    if (incognito) return 0;
    std::wstring n = CleanFolderName(name);
    std::wstring p = CleanParentPath(parent);
    if (n.empty()) return 0;
    Excl x;
    if (FolderExistsLocked(p, n)) return 0;
    EnsureFolderLocked(p, UnixMs());
    Entry f;
    f.id = nextId++; f.time = UnixMs(); f.folder = true;
    f.title = n; f.parent = p;
    bookmarks.push_back(std::move(f));
    bmDirty = true;
    return bookmarks.back().id;
}

bool DeleteBookmark(long long id) {
    Excl x;
    if (incognito) return false;
    for (size_t i = 0; i < bookmarks.size(); i++) {
        if (bookmarks[i].id != id) continue;
        if (!bookmarks[i].folder) {
            bookmarks.erase(bookmarks.begin() + i);
            bmDirty = true;
            return true;
        }
        std::wstring path = JoinPath(bookmarks[i].parent, bookmarks[i].title);
        std::wstring prefix = Lower(path) + L"/";
        std::wstring lowPath = Lower(path);
        bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(), [&](const Entry& e) {
            if (e.id == id) return true;
            std::wstring ep = Lower(e.parent);
            return ep == lowPath || ep.rfind(prefix, 0) == 0;
        }), bookmarks.end());
        bmDirty = true;
        return true;
    }
    return false;
}

bool EditBookmark(long long id, const std::wstring& title, const std::wstring& url,
                  const std::wstring& parent, bool updateParent) {
    if (incognito) return false;
    Excl x;
    size_t idx = bookmarks.size();
    for (size_t i = 0; i < bookmarks.size(); i++) if (bookmarks[i].id == id) { idx = i; break; }
    if (idx == bookmarks.size()) return false;

    std::wstring targetParent = updateParent ? CleanParentPath(parent) : bookmarks[idx].parent;
    if (bookmarks[idx].folder) {
        std::wstring newName = CleanFolderName(title);
        if (newName.empty()) return false;
        std::wstring oldPath = JoinPath(bookmarks[idx].parent, bookmarks[idx].title);
        std::wstring lowOld = Lower(oldPath), lowTarget = Lower(targetParent);
        if (lowTarget == lowOld || lowTarget.rfind(lowOld + L"/", 0) == 0) return false;
        if (FolderExistsLocked(targetParent, newName, id)) return false;
        EnsureFolderLocked(targetParent, UnixMs());
        bookmarks[idx].title = newName;
        bookmarks[idx].parent = targetParent;
        std::wstring newPath = JoinPath(targetParent, newName);
        if (newPath != oldPath) {
            std::wstring oldPrefix = lowOld + L"/";
            for (Entry& e : bookmarks) {
                std::wstring ep = Lower(e.parent);
                if (ep == lowOld) e.parent = newPath;
                else if (ep.rfind(oldPrefix, 0) == 0) e.parent = newPath + e.parent.substr(oldPath.size());
            }
        }
        bmDirty = true;
        return true;
    }
    if (!Recordable(url)) return false;
    EnsureFolderLocked(targetParent, UnixMs());
    bookmarks[idx].title = Clean(title.empty() ? url : title, 500);
    bookmarks[idx].url = url;
    bookmarks[idx].parent = targetParent;
    bmDirty = true;
    return true;
}

bool EditBookmark(long long id, const std::wstring& title, const std::wstring& url) {
    return EditBookmark(id, title, url, L"", false);
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

std::string BookmarksBarJson(const std::wstring& parent) {
    std::wstring p = Lower(CleanParentPath(parent));
    std::string o = "[";
    bool first = true;
    Shared s;
    for (const Entry& e : bookmarks) {
        if (Lower(e.parent) != p) continue;
        if (!first) o += ',';
        first = false;
        o += "{\"id\":" + std::to_string(e.id) + ",\"folder\":" + (e.folder ? "true" : "false") +
             ",\"title\":";
        Json(o, e.title);
        o += ",\"url\":";
        Json(o, e.url);
        o += ",\"path\":";
        Json(o, e.folder ? JoinPath(e.parent, e.title) : L"");
        o += '}';
    }
    return o + "]";
}

static void ExportFolderHtml(std::string& o, const std::vector<Entry>& all, const std::wstring& parent, int depth) {
    std::string pad(depth * 4, ' ');
    std::wstring lowP = Lower(parent);
    for (const Entry& e : all) {
        if (Lower(e.parent) != lowP) continue;
        std::string sec = std::to_string(std::max(0LL, e.time / 1000));
        if (e.folder) {
            o += pad + "<DT><H3 ADD_DATE=\"" + sec + "\" LAST_MODIFIED=\"" + sec + "\">" + HtmlEsc(e.title) + "</H3>\n";
            o += pad + "<DL><p>\n";
            ExportFolderHtml(o, all, JoinPath(e.parent, e.title), depth + 1);
            o += pad + "</DL><p>\n";
        } else {
            o += pad + "<DT><A HREF=\"" + HtmlEsc(e.url) + "\" ADD_DATE=\"" + sec + "\">" + HtmlEsc(e.title.empty() ? e.url : e.title) + "</A>\n";
        }
    }
}

std::string ExportBookmarksHtml() {
    std::vector<Entry> snap;
    {
        Shared s;
        snap = bookmarks;
    }
    std::string now = std::to_string(std::max(0LL, UnixMs() / 1000));
    std::string o =
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<!-- This is an automatically generated file.\n"
        "     It will be read and overwritten.\n"
        "     DO NOT EDIT! -->\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n"
        "<H1>Bookmarks</H1>\n"
        "<DL><p>\n"
        "    <DT><H3 ADD_DATE=\"" + now + "\" LAST_MODIFIED=\"" + now + "\" PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks bar</H3>\n"
        "    <DL><p>\n";
    ExportFolderHtml(o, snap, L"", 2);
    o += "    </DL><p>\n</DL><p>\n";
    return o;
}

bool ImportBookmarksHtml(const std::string& html, int& addedBookmarks, int& addedFolders) {
    addedBookmarks = 0; addedFolders = 0;
    if (incognito) return false;
    std::wstring w = DecodeHtmlBytes(html);
    if (w.empty()) return false;

    std::vector<std::wstring> stack;
    std::wstring pendingFolder;
    bool pendingToolbar = false;
    long long pendingTime = 0;
    long long now = UnixMs();

    Excl x;
    auto hasBookmark = [&](const std::wstring& url, const std::wstring& parent) {
        std::wstring lowP = Lower(parent);
        for (const Entry& e : bookmarks) if (!e.folder && e.url == url && Lower(e.parent) == lowP) return true;
        return false;
    };

    for (size_t i = 0; i < w.size();) {
        size_t lt = w.find(L'<', i);
        if (lt == std::wstring::npos) break;
        size_t gt = w.find(L'>', lt + 1);
        if (gt == std::wstring::npos) break;
        std::wstring tag = w.substr(lt + 1, gt - lt - 1);
        i = gt + 1;

        size_t n0 = 0;
        while (n0 < tag.size() && iswspace(tag[n0])) n0++;
        size_t n1 = n0;
        while (n1 < tag.size() && !iswspace(tag[n1]) && tag[n1] != L'/') n1++;
        std::wstring name = Lower(tag.substr(n0, n1 - n0));
        if (name.empty() && n0 < tag.size() && tag[n0] == L'/') {
            n1 = n0 + 1;
            while (n1 < tag.size() && !iswspace(tag[n1])) n1++;
            name = Lower(tag.substr(n0, n1 - n0));
        }

        if (name == L"h3") {
            size_t close = std::wstring::npos;
            for (size_t p = i; (p = w.find(L'<', p)) != std::wstring::npos; p++) {
                if (p + 4 < w.size() && Lower(w.substr(p, 4)) == L"</h3") { close = p; break; }
            }
            if (close != std::wstring::npos) {
                pendingFolder = CleanFolderName(StripTags(w.substr(i, close - i)));
                pendingToolbar = Lower(HtmlAttr(tag, L"PERSONAL_TOOLBAR_FOLDER")) == L"true";
                long long sec = _wtoi64(HtmlAttr(tag, L"ADD_DATE").c_str());
                pendingTime = sec > 0 ? sec * 1000 : now;
                size_t endGt = w.find(L'>', close);
                if (endGt != std::wstring::npos) i = endGt + 1;
            }
        } else if (name == L"dl") {
            std::wstring cur = stack.empty() ? L"" : stack.back();
            if (!pendingFolder.empty()) {
                // Top-level PERSONAL_TOOLBAR_FOLDER maps directly to NeonNet's Bookmarks bar (root)
                if (pendingToolbar && cur.empty()) {
                    stack.push_back(L"");
                } else {
                    std::wstring nextPath = JoinPath(cur, pendingFolder);
                    addedFolders += EnsureFolderLocked(nextPath, pendingTime);
                    stack.push_back(nextPath);
                }
                pendingFolder.clear();
                pendingToolbar = false;
            } else {
                stack.push_back(cur);
            }
        } else if (name == L"/dl") {
            if (!stack.empty()) stack.pop_back();
            pendingFolder.clear();
            pendingToolbar = false;
        } else if (name == L"a") {
            size_t close = std::wstring::npos;
            for (size_t p = i; (p = w.find(L'<', p)) != std::wstring::npos; p++) {
                if (p + 3 < w.size() && Lower(w.substr(p, 3)) == L"</a") { close = p; break; }
            }
            if (close != std::wstring::npos) {
                std::wstring href = TrimW(HtmlAttr(tag, L"href"));
                std::wstring title = StripTags(w.substr(i, close - i));
                long long sec = _wtoi64(HtmlAttr(tag, L"ADD_DATE").c_str());
                long long tm = sec > 0 ? sec * 1000 : now;
                std::wstring cur = stack.empty() ? L"" : stack.back();
                if (Recordable(href) && !hasBookmark(href, cur)) {
                    addedFolders += EnsureFolderLocked(cur, tm);
                    Entry e; e.id = nextId++; e.time = tm; e.url = href;
                    e.title = Clean(title.empty() ? href : title, 500);
                    e.parent = cur;
                    bookmarks.push_back(std::move(e));
                    addedBookmarks++;
                }
                size_t endGt = w.find(L'>', close);
                if (endGt != std::wstring::npos) i = endGt + 1;
            }
        }
    }
    if (addedBookmarks > 0 || addedFolders > 0) bmDirty = true;
    return true;
}

// Address bar suggestions: bookmarks and history entries that contain every word of the
// query in their address or title, one row per address, best first. The score favours an
// address that starts with the typed text, then bookmarks, then pages visited often and lately.
std::string SuggestJson(const std::wstring& query, size_t limit) {
    std::vector<std::wstring> tokens;
    {
        std::wstring q = Lower(query), cur;
        for (wchar_t c : q) {
            if (c == L' ' || c == L'\t') { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty() || tokens.size() > 5 || query.size() > 200) return "[]";
    std::wstring typed = Bare(query);

    struct Cand { std::wstring url, title; bool bookmark = false; int count = 0; long long last = 0; double score = 0; };
    std::vector<Cand> cands;
    std::unordered_map<std::wstring, size_t> index;
    auto matches = [&](const std::wstring& url, const std::wstring& title) {
        for (const auto& t : tokens) if (!ContainsI(url, t) && !ContainsI(title, t)) return false;
        return true;
    };
    auto slot = [&](const std::wstring& url) -> Cand& {
        std::wstring key = url;
        while (!key.empty() && key.back() == L'/') key.pop_back();
        auto it = index.find(key);
        if (it != index.end()) return cands[it->second];
        index[key] = cands.size();
        cands.emplace_back();
        cands.back().url = url;
        return cands.back();
    };

    Shared s;
    for (const Entry& e : bookmarks) {
        if (e.folder || !matches(e.url, e.title)) continue;
        Cand& c = slot(e.url);
        c.bookmark = true;
        if (!e.title.empty()) c.title = e.title;
    }
    for (const Entry& e : history) {
        if (!matches(e.url, e.title)) continue;
        Cand& c = slot(e.url);
        c.count++;
        if (e.time > c.last) { c.last = e.time; if (!e.title.empty() && c.title.empty()) c.title = e.title; }
    }

    long long now = UnixMs();
    for (Cand& c : cands) {
        std::wstring bare = Bare(c.url);
        std::wstring host = bare.substr(0, bare.find(L'/'));
        double sc = 0;
        if (!typed.empty() && bare.rfind(typed, 0) == 0) sc += 60;
        else if (host.rfind(tokens[0], 0) == 0) sc += 45;
        else if (host.find(tokens[0]) != std::wstring::npos) sc += 25;
        std::wstring spaced = L" " + c.title;
        for (const auto& t : tokens) if (ContainsI(spaced, L" " + t)) { sc += 8; break; }
        if (c.bookmark) sc += 25;
        sc += std::min(c.count, 25) * 1.2;
        if (c.last > 0) sc += 20.0 * std::exp(-((now - c.last) / 86400000.0) / 10.0);
        c.score = sc;
    }
    size_t n = std::min(limit, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + n, cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });

    std::string o = "[";
    for (size_t i = 0; i < n; i++) {
        if (i) o += ',';
        o += "{\"bookmark\":";
        o += cands[i].bookmark ? "true" : "false";
        o += ",\"url\":";
        Json(o, cands[i].url);
        o += ",\"title\":";
        Json(o, cands[i].title);
        o += '}';
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
