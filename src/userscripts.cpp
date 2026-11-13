#include "userscripts.h"
#include "resource.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace userscripts {
namespace {

std::mutex g_mu;
std::wstring g_dir;
bool g_readOnly = false;
long long g_nextId = 1;
std::vector<Script> g_scripts;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

std::string JsonStrUtf8(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o += (char)c;
            }
        }
    }
    o += "\"";
    return o;
}

std::string JsonStrW(const std::wstring& w) { return JsonStrUtf8(WideToUtf8(w)); }

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0 || sz.QuadPart > 4LL * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < (DWORD)out.size()) {
        if (!ReadFile(f, &out[total], (DWORD)out.size() - total, &rd, nullptr) || rd == 0) {
            CloseHandle(f);
            return false;
        }
        total += rd;
    }
    CloseHandle(f);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1024 * 1024);
        DWORD wr = 0;
        if (!WriteFile(f, data.data() + off, chunk, &wr, nullptr) || wr == 0) {
            CloseHandle(f);
            DeleteFileW(tmp.c_str());
            return false;
        }
        off += wr;
    }
    CloseHandle(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring ScriptFilePath(long long id) {
    return g_dir + L"\\" + std::to_wstring(id) + L".user.js";
}

bool SaveIndexLocked() {
    if (g_readOnly || g_dir.empty()) return false;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    std::string out = "NEONNET-USERSCRIPTS 1\n" + std::to_string(g_nextId) + "\n";
    for (const auto& s : g_scripts) {
        out += std::to_string(s.id) + "\t" + (s.enabled ? "1" : "0") + "\n";
    }
    return WriteFileAtomic(g_dir + L"\\index.txt", out);
}

bool LoadEmbeddedScript(int resId, std::string& out) {
    out.clear();
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&LoadEmbeddedScript, &hm)) return false;
    HRSRC hr = FindResourceW(hm, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(hm, hr);
    DWORD sz = hg ? SizeofResource(hm, hr) : 0;
    const void* ptr = hg ? LockResource(hg) : nullptr;
    if (!ptr || !sz) return false;
    out.assign((const char*)ptr, sz);
    return true;
}

void LoadLocked(const std::wstring& dataDir, bool readOnly) {
    g_scripts.clear();
    g_nextId = 1;
    g_readOnly = readOnly;
    g_dir = dataDir.empty() ? L"" : (dataDir + L"\\userscripts");
    if (g_dir.empty()) return;

    std::string idx;
    if (!ReadFileBytes(g_dir + L"\\index.txt", idx)) {
        // Seed built-in YouTube Ad Skipper on first run when index.txt does not exist yet.
        if (!readOnly && GetFileAttributesW((g_dir + L"\\index.txt").c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string code;
            if (LoadEmbeddedScript(IDR_YT_ADSKIP, code)) {
                CreateDirectoryW(dataDir.c_str(), nullptr);
                CreateDirectoryW(g_dir.c_str(), nullptr);
                Script s = ParseMeta(code);
                s.id = g_nextId++;
                s.enabled = true;
                s.code = std::move(code);
                if (WriteFileAtomic(ScriptFilePath(s.id), s.code)) {
                    g_scripts.push_back(std::move(s));
                    SaveIndexLocked();
                }
            }
        }
        return;
    }
    size_t pos = 0, nl = idx.find('\n');
    if (nl == std::string::npos) return;
    std::string hdr = idx.substr(0, nl);
    while (!hdr.empty() && hdr.back() == '\r') hdr.pop_back();
    if (hdr != "NEONNET-USERSCRIPTS 1") return;
    pos = nl + 1;

    nl = idx.find('\n', pos);
    if (nl != std::string::npos) {
        long long nxt = _atoi64(idx.substr(pos, nl - pos).c_str());
        if (nxt > 0) g_nextId = nxt;
        pos = nl + 1;
    }

    while (pos < idx.size()) {
        size_t end = idx.find('\n', pos);
        if (end == std::string::npos) end = idx.size();
        std::string line = idx.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        long long id = _atoi64(line.substr(0, tab).c_str());
        bool en = line.substr(tab + 1) == "1";
        if (id <= 0) continue;
        std::string code;
        if (!ReadFileBytes(ScriptFilePath(id), code)) continue;
        Script s = ParseMeta(code);
        s.id = id;
        s.enabled = en;
        s.code = std::move(code);
        g_scripts.push_back(std::move(s));
        if (id >= g_nextId) g_nextId = id + 1;
    }
}

bool WildcardMatch(const std::wstring& pat, const std::wstring& str, bool caseInsensitive) {
    size_t pi = 0, si = 0, star = std::wstring::npos, match = 0;
    while (si < str.size()) {
        if (pi < pat.size() && pat[pi] == L'*') {
            star = pi++;
            match = si;
        } else if (pi < pat.size() &&
                   (caseInsensitive ? towlower(pat[pi]) == towlower(str[si]) : pat[pi] == str[si])) {
            pi++;
            si++;
        } else if (star != std::wstring::npos) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == L'*') pi++;
    return pi == pat.size();
}

bool SplitHttpUrl(const std::wstring& url, std::wstring& scheme, std::wstring& host, std::wstring& path) {
    size_t sep = url.find(L"://");
    if (sep == std::wstring::npos) return false;
    scheme = LowerW(url.substr(0, sep));
    if (scheme != L"http" && scheme != L"https") return false;
    size_t hostStart = sep + 3;
    size_t slash = url.find_first_of(L"/?#", hostStart);
    std::wstring auth = slash == std::wstring::npos ? url.substr(hostStart) : url.substr(hostStart, slash - hostStart);
    size_t at = auth.rfind(L'@');
    if (at != std::wstring::npos) auth = auth.substr(at + 1);
    if (!auth.empty() && auth.front() == L'[') {
        size_t close = auth.find(L']');
        host = close == std::wstring::npos ? LowerW(auth) : LowerW(auth.substr(0, close + 1));
    } else {
        size_t colon = auth.rfind(L':');
        host = LowerW(colon == std::wstring::npos ? auth : auth.substr(0, colon));
    }
    if (host.empty()) return false;
    path = (slash == std::wstring::npos || url[slash] != L'/') ? L"/" : url.substr(slash);
    return true;
}

bool MatchPattern(const std::wstring& rawPat, const std::wstring& url) {
    std::wstring pat = TrimW(rawPat);
    if (pat.empty()) return false;
    if (pat == L"<all_urls>" || pat == L"*") return true;

    std::wstring uScheme, uHost, uPath;
    if (!SplitHttpUrl(url, uScheme, uHost, uPath)) return false;

    size_t sep = pat.find(L"://");
    if (sep == std::wstring::npos) return WildcardMatch(pat, url, true);

    std::wstring pScheme = LowerW(pat.substr(0, sep));
    if (pScheme == L"*") {
        // matches http and https
    } else if (pScheme.find(L'*') != std::wstring::npos) {
        if (!WildcardMatch(pScheme, uScheme, true)) return false;
    } else if (pScheme != uScheme) {
        return false;
    }

    size_t restStart = sep + 3;
    size_t slash = pat.find(L'/', restStart);
    std::wstring pHost = LowerW(slash == std::wstring::npos ? pat.substr(restStart) : pat.substr(restStart, slash - restStart));
    std::wstring pPath = slash == std::wstring::npos ? L"/*" : pat.substr(slash);
    if (pHost.empty()) return false;

    // Strip optional port in pattern host if any (when not IPv6 bracketed)
    if (pHost.front() != L'[') {
        size_t colon = pHost.rfind(L':');
        if (colon != std::wstring::npos) pHost.resize(colon);
    }

    bool hostOk = false;
    if (pHost == L"*") {
        hostOk = true;
    } else if (pHost.rfind(L"*.", 0) == 0 && pHost.find(L'*', 2) == std::wstring::npos) {
        std::wstring base = pHost.substr(2);
        if (uHost == base) hostOk = true;
        else if (uHost.size() > base.size() + 1 &&
                 uHost.compare(uHost.size() - base.size(), base.size(), base) == 0 &&
                 uHost[uHost.size() - base.size() - 1] == L'.') {
            hostOk = true;
        }
    } else if (pHost.find(L'*') != std::wstring::npos) {
        hostOk = WildcardMatch(pHost, uHost, true);
    } else {
        hostOk = (pHost == uHost);
    }
    if (!hostOk) return false;

    return WildcardMatch(pPath, uPath, false);
}

std::string SanitizeLabelUtf8(const std::wstring& name) {
    std::string u = WideToUtf8(name);
    std::string out;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_') out += (char)c;
        else if (c == ' ' && !out.empty() && out.back() != '_') out += '_';
        if (out.size() >= 48) break;
    }
    return out.empty() ? "script" : out;
}

std::string WrapScript(const Script& s, const wchar_t* appVersion) {
    std::string ver = WideToUtf8(appVersion ? appVersion : L"");
    std::string out;
    out.reserve(s.code.size() + 2048);
    out += "(function(){\n"
           "  var w = window;\n"
           "  var reg = w.__nnUs;\n"
           "  if (!reg) { reg = Object.create(null); try { Object.defineProperty(w, '__nnUs', { value: reg }); } catch(e) { w.__nnUs = reg; } }\n"
           "  var key = ";
    out += std::to_string(s.id);
    out += ";\n"
           "  if (reg[key]) return;\n"
           "  reg[key] = 1;\n"
           "  var unsafeWindow = w;\n"
           "  var GM_info = { script: { name: " + JsonStrW(s.name) +
           ", version: " + JsonStrW(s.version) +
           ", description: " + JsonStrW(s.description) +
           ", namespace: " + JsonStrW(s.namespaceUri) +
           ", author: " + JsonStrW(s.author) +
           " }, scriptHandler: 'NeonNet', version: " + JsonStrUtf8(ver) + " };\n"
           "  var __pfx = '__nn_gm_' + key + '_';\n"
           "  function GM_addStyle(css){\n"
           "    var st = document.createElement('style');\n"
           "    st.textContent = String(css || '');\n"
           "    var tgt = document.head || document.documentElement;\n"
           "    if (tgt) tgt.appendChild(st);\n"
           "    else document.addEventListener('DOMContentLoaded', function(){ (document.head || document.documentElement).appendChild(st); }, { once: true });\n"
           "    return st;\n"
           "  }\n"
           "  function GM_getValue(k, def){\n"
           "    try { var raw = localStorage.getItem(__pfx + k); return raw === null ? def : JSON.parse(raw); } catch(e){ return def; }\n"
           "  }\n"
           "  function GM_setValue(k, v){\n"
           "    try { localStorage.setItem(__pfx + k, JSON.stringify(v)); } catch(e){}\n"
           "  }\n"
           "  function GM_deleteValue(k){\n"
           "    try { localStorage.removeItem(__pfx + k); } catch(e){}\n"
           "  }\n"
           "  function GM_listValues(){\n"
           "    var out = [];\n"
           "    try { for (var i = 0; i < localStorage.length; i++){ var k = localStorage.key(i); if (k && k.indexOf(__pfx) === 0) out.push(k.slice(__pfx.length)); } } catch(e){}\n"
           "    return out;\n"
           "  }\n"
           "  function GM_openInTab(u){ return w.open(String(u), '_blank', 'noopener'); }\n"
           "  var GM = {\n"
           "    info: GM_info,\n"
           "    addStyle: GM_addStyle,\n"
           "    openInTab: GM_openInTab,\n"
           "    getValue: function(k, d){ return Promise.resolve(GM_getValue(k, d)); },\n"
           "    setValue: function(k, v){ GM_setValue(k, v); return Promise.resolve(); },\n"
           "    deleteValue: function(k){ GM_deleteValue(k); return Promise.resolve(); },\n"
           "    listValues: function(){ return Promise.resolve(GM_listValues()); }\n"
           "  };\n"
           "  function __run(){\n"
           "    try {\n";
    out += s.code;
    out += "\n    } catch(e) { console.error('[NeonNet Userscript] ' + GM_info.script.name + ':', e); }\n"
           "  }\n";
    if (s.runAt == RUN_IDLE) {
        out += "  setTimeout(__run, 0);\n";
    } else {
        out += "  __run();\n";
    }
    out += "})();\n";
    return out;
}

std::string ScriptToJson(const Script& s) {
    std::string o = "{\"id\":" + std::to_string(s.id) +
                    ",\"enabled\":" + (s.enabled ? "true" : "false") +
                    ",\"name\":" + JsonStrW(s.name) +
                    ",\"namespace\":" + JsonStrW(s.namespaceUri) +
                    ",\"version\":" + JsonStrW(s.version) +
                    ",\"author\":" + JsonStrW(s.author) +
                    ",\"description\":" + JsonStrW(s.description) +
                    ",\"runAt\":" + JsonStrUtf8(s.runAt == RUN_START ? "document-start" : s.runAt == RUN_IDLE ? "document-idle" : "document-end");
    auto addArr = [&](const char* k, const std::vector<std::wstring>& v) {
        o += ",\""; o += k; o += "\":[";
        for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += JsonStrW(v[i]); }
        o += "]";
    };
    addArr("matches", s.matches);
    addArr("includes", s.includes);
    addArr("excludes", s.excludes);
    addArr("grants", s.grants);
    o += ",\"code\":" + JsonStrUtf8(s.code) + "}";
    return o;
}

// Tiny no-exception JSON helpers for flat {"id":..., "enabled":..., "code":"..."} payloads.
const char* SkipWs(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

bool ParseJsonStringAt(const char*& p, const char* end, std::string& out) {
    out.clear();
    if (p >= end || *p != '"') return false;
    p++;
    auto appendUtf8 = [&](unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    };
    auto hex4 = [&](const char* q, unsigned& val) -> bool {
        if (q + 4 > end) return false;
        val = 0;
        for (int i = 0; i < 4; i++) {
            char c = q[i];
            unsigned d = 0;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return false;
            val = (val << 4) | d;
        }
        return true;
    };
    while (p < end) {
        char c = *p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(p, cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0;
                    if (hex4(p + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        p += 6;
                    }
                }
                appendUtf8(cp);
                break;
            }
            default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool SkipJsonValue(const char*& p, const char* end) {
    p = SkipWs(p, end);
    if (p >= end) return false;
    if (*p == '"') { std::string dummy; return ParseJsonStringAt(p, end, dummy); }
    if (*p == '{' || *p == '[') {
        char open = *p++, close = open == '{' ? '}' : ']';
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"') { std::string dummy; if (!ParseJsonStringAt(p, end, dummy)) return false; }
            else {
                if (*p == open) depth++;
                else if (*p == close) depth--;
                p++;
            }
        }
        return depth == 0;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return true;
}

bool ParseSavePayload(const std::string& body, long long& idOut, bool& hasEnabled, bool& enabledOut, bool& hasCode, std::string& codeOut) {
    idOut = 0; hasEnabled = false; enabledOut = true; hasCode = false; codeOut.clear();
    const char* p = body.data();
    const char* end = body.data() + body.size();
    p = SkipWs(p, end);
    if (p >= end || *p != '{') return false;
    p++;
    while (p < end) {
        p = SkipWs(p, end);
        if (p < end && *p == '}') return true;
        std::string key;
        if (!ParseJsonStringAt(p, end, key)) return false;
        p = SkipWs(p, end);
        if (p >= end || *p != ':') return false;
        p = SkipWs(p + 1, end);
        if (p >= end) return false;
        if (key == "id") {
            const char* numStart = p;
            if (*p == '-') p++;
            while (p < end && isdigit((unsigned char)*p)) p++;
            if (p == numStart) return false;
            idOut = _atoi64(std::string(numStart, p).c_str());
        } else if (key == "enabled") {
            if (end - p >= 4 && memcmp(p, "true", 4) == 0) { enabledOut = true; hasEnabled = true; p += 4; }
            else if (end - p >= 5 && memcmp(p, "false", 5) == 0) { enabledOut = false; hasEnabled = true; p += 5; }
            else return false;
        } else if (key == "code") {
            if (!ParseJsonStringAt(p, end, codeOut)) return false;
            hasCode = true;
        } else {
            if (!SkipJsonValue(p, end)) return false;
        }
        p = SkipWs(p, end);
        if (p < end && *p == ',') p++;
        else if (p < end && *p == '}') return true;
        else return false;
    }
    return false;
}

}  // namespace

void Load(const std::wstring& dataDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked(dataDir, false);
}

void LoadReadOnly(const std::wstring& dataDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked(dataDir, true);
}

bool IsReadOnly() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_readOnly;
}

Script ParseMeta(const std::string& utf8Code) {
    Script s;
    std::wstring w = Utf8ToWide(utf8Code);
    size_t pos = 0;
    bool inBlock = false;
    while (pos < w.size()) {
        size_t end = w.find(L'\n', pos);
        if (end == std::wstring::npos) end = w.size();
        std::wstring line = TrimW(w.substr(pos, end - pos));
        pos = end + 1;

        if (!inBlock) {
            if (line.rfind(L"// ==UserScript==", 0) == 0) inBlock = true;
            continue;
        }
        if (line.rfind(L"// ==/UserScript==", 0) == 0) break;
        if (line.rfind(L"//", 0) != 0) continue;
        std::wstring body = TrimW(line.substr(2));
        if (body.empty() || body[0] != L'@') continue;
        size_t sp = body.find_first_of(L" \t");
        std::wstring key = LowerW(sp == std::wstring::npos ? body.substr(1) : body.substr(1, sp - 1));
        std::wstring val = sp == std::wstring::npos ? L"" : TrimW(body.substr(sp + 1));
        if (key == L"name" && !val.empty()) s.name = val;
        else if (key == L"namespace") s.namespaceUri = val;
        else if (key == L"version") s.version = val;
        else if (key == L"author") s.author = val;
        else if (key == L"description") s.description = val;
        else if (key == L"run-at") {
            std::wstring v = LowerW(val);
            if (v == L"document-start") s.runAt = RUN_START;
            else if (v == L"document-idle") s.runAt = RUN_IDLE;
            else s.runAt = RUN_END;
        } else if (key == L"match" && !val.empty()) s.matches.push_back(val);
        else if (key == L"include" && !val.empty()) s.includes.push_back(val);
        else if (key == L"exclude" && !val.empty()) s.excludes.push_back(val);
        else if (key == L"grant" && !val.empty()) s.grants.push_back(val);
    }
    if (s.name.empty()) s.name = L"Untitled script";
    return s;
}

bool MatchesUrl(const Script& s, const std::wstring& url) {
    std::wstring low = LowerW(url);
    if (low.rfind(L"http://", 0) != 0 && low.rfind(L"https://", 0) != 0) return false;
    // Never allow userscripts to run on NeonNet's internal privileged UI pages.
    if (low.rfind(L"https://neonnet.ui/", 0) == 0 || low.rfind(L"https://chrome.neonnet.ui/", 0) == 0) return false;

    for (const auto& ex : s.excludes) {
        if (MatchPattern(ex, url)) return false;
    }
    for (const auto& m : s.matches) {
        if (MatchPattern(m, url)) return true;
    }
    for (const auto& inc : s.includes) {
        if (MatchPattern(inc, url)) return true;
    }
    return false;
}

std::vector<Injected> ScriptsForUrl(const std::wstring& url, RunAt phase, const wchar_t* appVersion) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<Injected> out;
    for (const auto& s : g_scripts) {
        if (!s.enabled || s.runAt != phase) continue;
        if (!MatchesUrl(s, url)) continue;
        Injected inj;
        inj.id = s.id;
        inj.label = L"neonnet://userscript/" + std::to_wstring(s.id) + L"/" + Utf8ToWide(SanitizeLabelUtf8(s.name)) + L".user.js";
        inj.js = WrapScript(s, appVersion);
        out.push_back(std::move(inj));
    }
    return out;
}

void Counts(size_t& totalOut, size_t& enabledOut) {
    std::lock_guard<std::mutex> lk(g_mu);
    totalOut = g_scripts.size();
    enabledOut = 0;
    for (const auto& s : g_scripts) if (s.enabled) enabledOut++;
}

std::string ListJson() {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t enabled = 0;
    for (const auto& s : g_scripts) if (s.enabled) enabled++;
    std::string o = std::string("{\"incognito\":") + (g_readOnly ? "true" : "false") +
                    ",\"total\":" + std::to_string(g_scripts.size()) +
                    ",\"enabled\":" + std::to_string(enabled) + ",\"scripts\":[";
    for (size_t i = 0; i < g_scripts.size(); i++) {
        if (i) o += ",";
        o += ScriptToJson(g_scripts[i]);
    }
    o += "]}";
    return o;
}

long long SaveFromJson(const std::string& bodyUtf8, std::string& errOut) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_readOnly) { errOut = "userscripts cannot be changed in incognito mode"; return 0; }
    long long reqId = 0;
    bool hasEn = false, en = true, hasCode = false;
    std::string code;
    if (!ParseSavePayload(bodyUtf8, reqId, hasEn, en, hasCode, code)) {
        errOut = "invalid json payload";
        return 0;
    }
    if (!hasCode || code.empty()) {
        errOut = "script code cannot be empty";
        return 0;
    }
    if (code.size() > 2 * 1024 * 1024) {
        errOut = "script exceeds 2 MB limit";
        return 0;
    }
    Script parsed = ParseMeta(code);
    parsed.code = std::move(code);

    CreateDirectoryW(g_dir.c_str(), nullptr);

    if (reqId > 0) {
        for (auto& existing : g_scripts) {
            if (existing.id == reqId) {
                parsed.id = reqId;
                parsed.enabled = hasEn ? en : existing.enabled;
                if (!WriteFileAtomic(ScriptFilePath(parsed.id), parsed.code)) {
                    errOut = "could not write script file";
                    return 0;
                }
                existing = std::move(parsed);
                if (!SaveIndexLocked()) { errOut = "could not save index"; return 0; }
                return reqId;
            }
        }
        errOut = "script not found";
        return 0;
    }

    parsed.id = g_nextId++;
    parsed.enabled = hasEn ? en : true;
    if (!WriteFileAtomic(ScriptFilePath(parsed.id), parsed.code)) {
        errOut = "could not write script file";
        return 0;
    }
    long long newId = parsed.id;
    g_scripts.push_back(std::move(parsed));
    if (!SaveIndexLocked()) {
        g_scripts.pop_back();
        DeleteFileW(ScriptFilePath(newId).c_str());
        errOut = "could not save index";
        return 0;
    }
    return newId;
}

bool SetEnabled(long long id, bool enabled) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_readOnly || id <= 0) return false;
    for (auto& s : g_scripts) {
        if (s.id == id) {
            if (s.enabled == enabled) return true;
            s.enabled = enabled;
            return SaveIndexLocked();
        }
    }
    return false;
}

bool Delete(long long id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_readOnly || id <= 0) return false;
    for (size_t i = 0; i < g_scripts.size(); i++) {
        if (g_scripts[i].id == id) {
            g_scripts.erase(g_scripts.begin() + i);
            DeleteFileW(ScriptFilePath(id).c_str());
            return SaveIndexLocked();
        }
    }
    return false;
}

}  // namespace userscripts
