// JSON text for the answers NeonNet's own pages read.
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

// `w` as a quoted UTF-8 JSON string. "<" is escaped too, so the result is safe inside <script>.
inline std::string JsonUtf8(const std::wstring& w) {
    std::string s, o = "\"";
    if (!w.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        s.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    }
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '<') o += "\\u003c";
        else if (u < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", u); o += b; }
        else o += c;
    }
    return o + "\"";
}
