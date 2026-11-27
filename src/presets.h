// Fixed choices that both the settings module and the code that applies them need.
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

// Accent colour pairs. `id` is saved in the registry; "custom" is the user's own pair.
struct ThemePreset {
    const wchar_t* id;
    const wchar_t* name;
    const wchar_t* pink;
    const wchar_t* cyan;
    COLORREF pinkCol;
    COLORREF cyanCol;
};
inline constexpr ThemePreset THEME_PRESETS[] = {
    { L"synthwave",   L"Synthwave (Default)", L"#ff2e97", L"#00e5ff", RGB(0xff, 0x2e, 0x97), RGB(0x00, 0xe5, 0xff) },
    // Cyber Matrix is lime, not mint: mint (#35ffa0) is the ON / success colour and the incognito accent.
    { L"cybergreen",  L"Cyber Matrix",        L"#a6ff1f", L"#00e5ff", RGB(0xa6, 0xff, 0x1f), RGB(0x00, 0xe5, 0xff) },
    { L"plasma",      L"Electric Cyan",       L"#00e5ff", L"#b967ff", RGB(0x00, 0xe5, 0xff), RGB(0xb9, 0x67, 0xff) },
    // The secondary is blue, not yellow: #ffd600 is the chrome highlight (wordmark, headings, nav buttons).
    { L"amber",       L"Solar Amber",         L"#ff9a3c", L"#3db8ff", RGB(0xff, 0x9a, 0x3c), RGB(0x3d, 0xb8, 0xff) },
    // Scarlet, leaning orange: the danger red (#ff4d6d) is pinkish, and a pure crimson would look like it.
    { L"crimson",     L"Crimson Edge",        L"#ff5a1f", L"#00e5ff", RGB(0xff, 0x5a, 0x1f), RGB(0x00, 0xe5, 0xff) },
    { L"ultraviolet", L"Ultraviolet",         L"#c74cff", L"#00e5ff", RGB(0xc7, 0x4c, 0xff), RGB(0x00, 0xe5, 0xff) },
};
inline constexpr int THEME_PRESET_COUNT = (int)(sizeof(THEME_PRESETS) / sizeof(THEME_PRESETS[0]));

// DNS-over-HTTPS (DoH) preset providers. `id` is saved in the registry.
struct DohProvider { const wchar_t* id; const wchar_t* name; const wchar_t* tmpl; };
inline constexpr DohProvider DOH_PROVIDERS[] = {
    { L"cloudflare", L"Cloudflare (1.1.1.1)",       L"https://cloudflare-dns.com/dns-query" },
    { L"google",     L"Google (8.8.8.8)",           L"https://dns.google/dns-query" },
    { L"quad9",      L"Quad9 (Security)",           L"https://dns.quad9.net/dns-query" },
    { L"adguard",    L"AdGuard DNS (Ad-blocking)",  L"https://dns.adguard-dns.com/dns-query" },
    { L"opendns",    L"Cisco OpenDNS",              L"https://doh.opendns.com/dns-query" },
    { L"custom",     L"Custom URL",                 L"" },
};
inline constexpr int DOH_PROVIDER_COUNT = (int)(sizeof(DOH_PROVIDERS) / sizeof(DOH_PROVIDERS[0]));

// "#rrggbb" in either case. `normOut` is the lower case form.
inline bool ParseHexColor(const std::wstring& in, COLORREF& colOut, std::wstring& normOut) {
    if (in.size() != 7 || in[0] != L'#') return false;
    unsigned int val = 0;
    for (int i = 1; i < 7; i++) {
        wchar_t c = in[i];
        int d = -1;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = 10 + (c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10 + (c - L'A');
        else return false;
        val = (val << 4) | (unsigned int)d;
    }
    BYTE r = (BYTE)((val >> 16) & 0xff), g = (BYTE)((val >> 8) & 0xff), b = (BYTE)(val & 0xff);
    colOut = RGB(r, g, b);
    wchar_t buf[16];
    swprintf_s(buf, L"#%02x%02x%02x", r, g, b);
    normOut = buf;
    return true;
}
