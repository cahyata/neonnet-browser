#include "updater.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace upd {

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v) : h(v) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Reads the JSON string value that follows "key": starting at |from|. Handles the
// escapes GitHub actually emits (\" \\ \/ \n \t \uXXXX for ASCII).
bool ReadString(const std::string& json, const std::string& key, size_t from, std::string& out, size_t* next) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return false;
    size_t i = k + needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
    if (i >= json.size() || json[i] != ':') return false;
    i++;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
    if (i >= json.size() || json[i] != '"') return false;
    i++;
    out.clear();
    for (; i < json.size(); i++) {
        char c = json[i];
        if (c == '"') { if (next) *next = i + 1; return true; }
        if (c == '\\' && i + 1 < json.size()) {
            char e = json[++i];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u':
                if (i + 4 < json.size()) {
                    unsigned v = strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
                    out += v < 0x80 ? (char)v : '?';
                    i += 4;
                }
                break;
            default: out += e;  // \" \\ \/
            }
        } else {
            out += c;
        }
    }
    return false;
}

}  // namespace

bool HttpGet(const std::wstring& url, std::string* body, HANDLE file, size_t maxBody,
             const Progress& progress, const std::atomic<bool>* cancel, DWORD* status) {
    if (status) *status = 0;
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    Handle session(WinHttpOpen(L"NeonNet-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    Handle conn(WinHttpConnect(session, host, uc.nPort, 0));
    if (!conn) return false;
    std::wstring object = std::wstring(path) + extra;
    Handle req(WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req) return false;
    // follows redirects (GitHub release assets redirect to a CDN); never HTTPS -> HTTP
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(req, nullptr)) return false;

    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &len, WINHTTP_NO_HEADER_INDEX);
    if (status) *status = code;
    if (code != 200) return false;

    ULONGLONG total = 0;
    wchar_t clen[32] = {}; DWORD clenSize = sizeof(clen);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &clenSize, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, nullptr, 10);

    std::vector<char> buf(64 * 1024);
    ULONGLONG received = 0;
    for (;;) {
        if (cancel && cancel->load()) return false;
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) return false;
        if (got == 0) break;
        received += got;
        if (body) {
            if (body->size() + got > maxBody) return false;
            body->append(buf.data(), got);
        }
        if (file) {
            DWORD wr = 0;
            if (!WriteFile(file, buf.data(), got, &wr, nullptr) || wr != got) return false;
        }
        if (progress) progress(received, total);
    }
    return total == 0 || received == total;
}

bool ParseLatest(const std::string& json, const std::wstring& repo, Release& out, std::wstring& err) {
    std::string tag, page;
    if (!ReadString(json, "tag_name", 0, tag, nullptr) || tag.empty()) { err = L"release has no tag"; return false; }
    ReadString(json, "html_url", 0, page, nullptr);
    out.tag = Widen(tag);
    out.version = (out.tag[0] == L'v' || out.tag[0] == L'V') ? out.tag.substr(1) : out.tag;
    out.pageUrl = Widen(page);

    // Only accept assets attached to this exact release of this exact repository.
    std::wstring base = L"https://github.com/" + repo + L"/releases/download/" + out.tag + L"/";
    std::wstring wantExe = base + L"NeonNet-Setup.exe", wantSha = wantExe + L".sha256";
    out.installerUrl.clear(); out.shaUrl.clear();
    size_t pos = 0;
    std::string url;
    while (ReadString(json, "browser_download_url", pos, url, &pos)) {
        std::wstring w = Widen(url);
        if (_wcsicmp(w.c_str(), wantExe.c_str()) == 0) out.installerUrl = w;
        else if (_wcsicmp(w.c_str(), wantSha.c_str()) == 0) out.shaUrl = w;
    }
    if (out.installerUrl.empty() || out.shaUrl.empty()) { err = L"release " + out.tag + L" has no installer or checksum"; return false; }
    if (out.pageUrl.rfind(L"https://github.com/" + repo + L"/", 0) != 0) out.pageUrl = L"https://github.com/" + repo + L"/releases/latest";
    return true;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    const wchar_t* pa = a.c_str();
    const wchar_t* pb = b.c_str();
    for (int i = 0; i < 4; i++) {
        wchar_t* ea; wchar_t* eb;
        unsigned long va = wcstoul(pa, &ea, 10), vb = wcstoul(pb, &eb, 10);
        if (va != vb) return va < vb ? -1 : 1;
        pa = (*ea == L'.') ? ea + 1 : ea;
        pb = (*eb == L'.') ? eb + 1 : eb;
        if (!*pa && !*pb) break;
    }
    return 0;
}

bool Sha256File(const std::wstring& path, std::string& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        ok = BCryptHashData(hash, buf.data(), got, 0) == 0;
    unsigned char digest[32];
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    static const char* hx = "0123456789abcdef";
    hex.clear();
    for (unsigned char c : digest) { hex += hx[c >> 4]; hex += hx[c & 15]; }
    return true;
}

std::string ParseShaFile(const std::string& text) {
    std::string h;
    for (char c : text) {
        if (isxdigit((unsigned char)c)) { h += (char)tolower((unsigned char)c); if (h.size() == 64) return h; }
        else if (!h.empty()) break;
    }
    return {};
}

}  // namespace upd
