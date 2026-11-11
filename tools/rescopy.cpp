// rescopy <source.dll> <target.exe>
// Replaces the icons and version info of target (the CEF bootstrap.exe renamed to
// NeonNet.exe) with the ones compiled into source (NeonNet.dll).

#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma pack(push, 2)
struct GrpEntry { BYTE w, h, colors, reserved; WORD planes, bpp; DWORD bytes; WORD id; };
struct GrpDir { WORD reserved, type, count; };
#pragma pack(pop)

struct Res { LPCWSTR type; std::wstring name; WORD nameId; bool isId; WORD lang; };

static std::vector<Res> g_list;

static BOOL CALLBACK EnumLang(HMODULE, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR) {
    Res r{ type, L"", 0, IS_INTRESOURCE(name) != 0, lang };
    if (r.isId) r.nameId = (WORD)(ULONG_PTR)name; else r.name = name;
    g_list.push_back(r);
    return TRUE;
}
static BOOL CALLBACK EnumName(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p) {
    EnumResourceLanguagesW(m, type, name, EnumLang, p);
    return TRUE;
}

static std::vector<BYTE> Load(HMODULE m, LPCWSTR type, LPCWSTR name) {
    HRSRC r = FindResourceW(m, name, type);
    if (!r) return {};
    const BYTE* p = (const BYTE*)LockResource(LoadResource(m, r));
    return std::vector<BYTE>(p, p + SizeofResource(m, r));
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { fwprintf(stderr, L"usage: rescopy <source.dll> <target.exe>\n"); return 2; }

    // 1. list existing icon/version resources in the target so they can be removed
    HMODULE tgt = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!tgt) { fwprintf(stderr, L"cannot open %s\n", argv[2]); return 1; }
    for (LPCWSTR t : { RT_GROUP_ICON, RT_ICON, RT_VERSION }) EnumResourceNamesW(tgt, t, EnumName, 0);
    std::vector<Res> old = g_list;
    FreeLibrary(tgt);

    // 2. read the replacement resources from the source
    HMODULE src = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!src) { fwprintf(stderr, L"cannot open %s\n", argv[1]); return 1; }
    std::vector<BYTE> group = Load(src, RT_GROUP_ICON, MAKEINTRESOURCEW(101));
    std::vector<BYTE> version = Load(src, RT_VERSION, MAKEINTRESOURCEW(1));
    if (group.size() < sizeof(GrpDir)) { fwprintf(stderr, L"source has no icon group 101\n"); return 1; }
    const GrpDir* dir = (const GrpDir*)group.data();
    const GrpEntry* ent = (const GrpEntry*)(group.data() + sizeof(GrpDir));
    std::vector<std::vector<BYTE>> icons;
    for (WORD i = 0; i < dir->count; i++) icons.push_back(Load(src, RT_ICON, MAKEINTRESOURCEW(ent[i].id)));
    FreeLibrary(src);

    // 3. rewrite the target
    HANDLE h = BeginUpdateResourceW(argv[2], FALSE);
    if (!h) { fwprintf(stderr, L"BeginUpdateResource failed %lu\n", GetLastError()); return 1; }
    for (auto& r : old)
        UpdateResourceW(h, r.type, r.isId ? MAKEINTRESOURCEW(r.nameId) : r.name.c_str(), r.lang, nullptr, 0);
    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    std::vector<BYTE> newGroup = group;
    GrpEntry* ne = (GrpEntry*)(newGroup.data() + sizeof(GrpDir));
    for (WORD i = 0; i < dir->count; i++) {
        ne[i].id = (WORD)(i + 1);
        UpdateResourceW(h, RT_ICON, MAKEINTRESOURCEW(i + 1), lang, icons[i].data(), (DWORD)icons[i].size());
    }
    UpdateResourceW(h, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang, newGroup.data(), (DWORD)newGroup.size());
    if (!version.empty()) UpdateResourceW(h, RT_VERSION, MAKEINTRESOURCEW(1), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), version.data(), (DWORD)version.size());
    if (!EndUpdateResourceW(h, FALSE)) { fwprintf(stderr, L"EndUpdateResource failed %lu\n", GetLastError()); return 1; }
    wprintf(L"rescopy: %u icons + version copied into %s\n", dir->count, argv[2]);
    return 0;
}
