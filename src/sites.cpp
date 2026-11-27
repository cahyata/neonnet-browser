#include "sites.h"
#include <windows.h>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <set>
#include <utility>

namespace sites {

void SplitUrl(const std::wstring& u, std::wstring& scheme, std::wstring& host, int& port) {
    scheme.clear(); host.clear(); port = 0;
    size_t s = u.find(L"://");
    if (s == std::wstring::npos) return;
    scheme = u.substr(0, s);
    for (auto& c : scheme) c = (wchar_t)towlower(c);
    std::wstring a = u.substr(s + 3);
    a = a.substr(0, a.find_first_of(L"/?#"));
    size_t at = a.rfind(L'@');
    if (at != std::wstring::npos) a = a.substr(at + 1);
    if (!a.empty() && a[0] == L'[') {
        size_t e = a.find(L']');
        if (e == std::wstring::npos) return;
        host = a.substr(1, e - 1);
        if (e + 1 < a.size() && a[e + 1] == L':') port = _wtoi(a.c_str() + e + 2);
    } else {
        size_t c = a.rfind(L':');
        if (c != std::wstring::npos) { port = _wtoi(a.c_str() + c + 1); a.resize(c); }
        host = a;
    }
    for (auto& c : host) c = (wchar_t)towlower(c);
}

std::wstring HostOf(const std::wstring& url) {
    std::wstring scheme, host; int port;
    SplitUrl(url, scheme, host, port);
    return host;
}

std::wstring OriginOf(const std::wstring& u) {
    std::wstring sc, h; int p;
    SplitUrl(u, sc, h, p);
    if (sc.empty() || h.empty()) return L"";
    std::wstring o = sc + L"://" + (h.find(L':') != std::wstring::npos ? L"[" + h + L"]" : h);
    if (p && !((sc == L"http" && p == 80) || (sc == L"https" && p == 443))) o += L":" + std::to_wstring(p);
    return o;
}

std::string HostFromAuthority(std::string a) {
    size_t at = a.rfind('@'); if (at != std::string::npos) a.erase(0, at + 1);
    if (!a.empty() && a[0] == '[') {
        size_t e = a.find(']');
        a = e == std::string::npos ? std::string() : a.substr(1, e - 1);
    } else {
        // two or more colons without brackets are an IPv6 address, not a port
        size_t colon = a.find(':');
        if (colon != std::string::npos && a.find(':', colon + 1) == std::string::npos) a.resize(colon);
    }
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    return a;
}

std::string_view SiteOf(std::string_view host) {
    // an IP address has no parent: it is a site of its own
    if (host.find(':') != std::string_view::npos || host.find_first_not_of("0123456789.") == std::string_view::npos) return host;
    size_t d1 = host.rfind('.');
    if (d1 == std::string_view::npos || d1 == 0) return host;
    size_t d2 = host.rfind('.', d1 - 1);
    if (d2 == std::string_view::npos) return host;
    static const std::string_view second[] = { "co", "com", "net", "org", "gov", "edu", "ac", "or", "go" };
    std::string_view tld = host.substr(d1 + 1);
    std::string_view sld = host.substr(d2 + 1, d1 - d2 - 1);
    bool two = tld.size() == 2;
    bool isSecond = false;
    for (auto x : second) if (sld == x) { isSecond = true; break; }
    if (two && isSecond) {
        if (d2 == 0) return host;
        size_t d3 = host.rfind('.', d2 - 1);
        return d3 == std::string_view::npos ? host : host.substr(d3 + 1);
    }
    return host.substr(d2 + 1);
}

namespace {

// In the order of List.
const wchar_t* const STORE[] = { L"AdsExemptHosts", L"TrackersExemptHosts", L"Cookie3pExemptHosts", L"ForceDarkExempt", L"KeepAwakeHosts" };
static_assert(sizeof(STORE) / sizeof(STORE[0]) == (size_t)List::Count, "one storage name per list");
const size_t MAX_ENTRY = 256;

std::mutex g_mu;   // request handlers on the IO thread ask about hosts
std::set<std::string> g_lists[(int)List::Count];
settings::Storage* g_storage = nullptr;
bool g_incognito = false;
Listener g_onChange;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Stored as one text, entries separated by ";". An empty list is not stored.
void Save(List list) {
    if (g_incognito || !g_storage) return;
    std::string joined;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& e : g_lists[(int)list]) { if (!joined.empty()) joined += ';'; joined += e; }
    }
    if (joined.empty()) g_storage->Erase(STORE[(int)list]);
    else g_storage->WriteText(STORE[(int)list], Widen(joined));
}

void Changed(List list, const std::vector<std::string>& entries, bool added) {
    Save(list);
    if (g_onChange) g_onChange(list, entries, added);
}

}  // namespace

void Load(settings::Storage& storage, bool incognito, Listener onChange) {
    g_storage = &storage;
    g_incognito = incognito;
    g_onChange = std::move(onChange);
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < (int)List::Count; i++) {
        g_lists[i].clear();
        std::wstring stored;
        if (!storage.ReadText(STORE[i], stored)) continue;
        std::string s = Narrow(stored);
        for (size_t p = 0; p < s.size();) {
            size_t e = s.find(';', p); if (e == std::string::npos) e = s.size();
            if (e > p && e - p < MAX_ENTRY) g_lists[i].insert(s.substr(p, e - p));
            p = e + 1;
        }
    }
}

std::string EntryFor(List list, std::string_view host) {
    std::string h(host);
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    switch (list) {
    case List::Ads: case List::Trackers: return std::string(SiteOf(h));
    case List::ForceDark: case List::KeepAwake:
        if (h.rfind("www.", 0) == 0) h.erase(0, 4);
        return h;
    default: return h;
    }
}

bool Excepted(List list, std::string_view host) {
    const std::string entry = EntryFor(list, host);
    if (entry.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_lists[(int)list].count(entry) > 0;
}

bool Toggle(List list, std::string_view host) {
    const std::string entry = EntryFor(list, host);
    if (entry.empty()) return false;
    bool added = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_lists[(int)list].erase(entry)) { g_lists[(int)list].insert(entry); added = true; }
    }
    Changed(list, { entry }, added);
    return added;
}

bool Add(List list, const std::wstring& typed) {
    std::string s = Narrow(typed);
    size_t scheme = s.find("://"); if (scheme != std::string::npos) s.erase(0, scheme + 3);
    size_t path = s.find_first_of("/?#"); if (path != std::string::npos) s.resize(path);
    const std::string entry = EntryFor(list, HostFromAuthority(s));
    // ";" separates the stored entries
    if (entry.empty() || entry.size() >= MAX_ENTRY || entry.find(';') != std::string::npos) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_lists[(int)list].insert(entry);
    }
    Changed(list, { entry }, true);   // also when it was there: the effect is applied again
    return true;
}

void Remove(List list, const std::wstring& entry) {
    const std::string e = Narrow(entry);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_lists[(int)list].erase(e)) return;
    }
    Changed(list, { e }, false);
}

void Clear(List list) {
    std::vector<std::string> removed;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        removed.assign(g_lists[(int)list].begin(), g_lists[(int)list].end());
        g_lists[(int)list].clear();
    }
    if (!removed.empty()) Changed(list, removed, false);
}

std::vector<std::wstring> Entries(List list) {
    std::vector<std::wstring> out;
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& e : g_lists[(int)list]) out.push_back(Widen(e));
    return out;
}

}  // namespace sites
