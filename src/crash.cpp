#include "crash.h"

#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace crash {
namespace {

wchar_t g_dir[MAX_PATH]{};
wchar_t g_logPath[MAX_PATH]{};
wchar_t g_prevPath[MAX_PATH]{};
char g_envLine[256]{};
bool g_inited = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::atomic<bool> g_busy{ false };

void Append(char* buf, size_t cap, size_t& pos, const char* fmt, ...) {
    if (pos + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) pos += (size_t)n < cap - pos ? (size_t)n : cap - pos - 1;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    case 0xE06D7363:                         return "CPP_EH_EXCEPTION";
    case 0x40000015:                         return "STATUS_FATAL_APP_EXIT";
    default:                                 return "UNKNOWN_EXCEPTION";
    }
}

void ModuleOffset(DWORD64 addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)addr, &mod) || !mod) {
        snprintf(out, cap, "<unknown>");
        return;
    }
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = path;
    for (DWORD i = 0; i < len; i++) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    char name[128]{};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
    DWORD64 off = addr - (DWORD64)(uintptr_t)mod;
    snprintf(out, cap, "%s+0x%llX", name[0] ? name : "<module>", (unsigned long long)off);
}

void WriteCrashFile(const char* text, size_t len) {
    if (!g_logPath[0] || !len) return;
    CreateDirectoryW(g_dir, nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fad)) {
        ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > 256ULL * 1024) MoveFileExW(g_logPath, g_prevPath, MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE f = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER zero{};
    SetFilePointerEx(f, zero, nullptr, FILE_END);
    DWORD wr = 0;
    WriteFile(f, text, (DWORD)len, &wr, nullptr);
    CloseHandle(f);
}

void WriteStack(char* buf, size_t cap, size_t& pos, const CONTEXT* ctxIn) {
    typedef BOOL(WINAPI* FnSymInit)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI* FnSymCleanup)(HANDLE);
    typedef BOOL(WINAPI* FnStackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                        PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                        PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    typedef PVOID(WINAPI* FnSymFuncTable)(HANDLE, DWORD64);
    typedef DWORD64(WINAPI* FnSymModBase)(HANDLE, DWORD64);
    typedef BOOL(WINAPI* FnSymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    typedef BOOL(WINAPI* FnSymGetLine)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    HANDLE proc = GetCurrentProcess();
    HANDLE thr = GetCurrentThread();
    bool symReady = false;
    FnSymInit symInit = nullptr;
    FnSymCleanup symCleanup = nullptr;
    FnStackWalk64 stackWalk = nullptr;
    FnSymFuncTable funcTable = nullptr;
    FnSymModBase modBase = nullptr;
    FnSymFromAddr symFromAddr = nullptr;
    FnSymGetLine symGetLine = nullptr;

    if (dbg) {
        symInit = (FnSymInit)GetProcAddress(dbg, "SymInitialize");
        symCleanup = (FnSymCleanup)GetProcAddress(dbg, "SymCleanup");
        stackWalk = (FnStackWalk64)GetProcAddress(dbg, "StackWalk64");
        funcTable = (FnSymFuncTable)GetProcAddress(dbg, "SymFunctionTableAccess64");
        modBase = (FnSymModBase)GetProcAddress(dbg, "SymGetModuleBase64");
        symFromAddr = (FnSymFromAddr)GetProcAddress(dbg, "SymFromAddr");
        symGetLine = (FnSymGetLine)GetProcAddress(dbg, "SymGetLineFromAddr64");
        if (symInit) symReady = symInit(proc, nullptr, TRUE) != FALSE;
    }

    int count = 0;
    if (ctxIn && stackWalk && funcTable && modBase) {
        CONTEXT ctx = *ctxIn;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 24; i++) {
            if (!stackWalk(IMAGE_FILE_MACHINE_AMD64, proc, thr, &frame, &ctx, nullptr, funcTable, modBase, nullptr)) break;
            DWORD64 pc = frame.AddrPC.Offset;
            if (!pc) break;
            char modOff[160];
            ModuleOffset(pc, modOff, sizeof(modOff));
            char symNote[256]{};
            if (symReady && symFromAddr) {
                alignas(SYMBOL_INFO) char symMem[sizeof(SYMBOL_INFO) + 128]{};
                auto* si = (SYMBOL_INFO*)symMem;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 127;
                DWORD64 disp = 0;
                if (symFromAddr(proc, pc, &disp, si)) {
                    IMAGEHLP_LINE64 ln{ sizeof(ln) };
                    DWORD ldisp = 0;
                    if (symGetLine && symGetLine(proc, pc, &ldisp, &ln) && ln.FileName) {
                        const char* fn = ln.FileName;
                        for (const char* p = fn; *p; p++) if (*p == '\\' || *p == '/') fn = p + 1;
                        snprintf(symNote, sizeof(symNote), "  %s+0x%llX (%s:%lu)", si->Name, (unsigned long long)disp, fn, ln.LineNumber);
                    } else {
                        snprintf(symNote, sizeof(symNote), "  %s+0x%llX", si->Name, (unsigned long long)disp);
                    }
                }
            }
            Append(buf, cap, pos, "  #%02d  0x%016llX  %s%s\n", count++, (unsigned long long)pc, modOff, symNote);
        }
    }

    if (count == 0) {
        void* frames[24]{};
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; i++) {
            char modOff[160];
            ModuleOffset((DWORD64)(uintptr_t)frames[i], modOff, sizeof(modOff));
            Append(buf, cap, pos, "  #%02u  0x%016llX  %s\n", i, (unsigned long long)(uintptr_t)frames[i], modOff);
        }
    }

    if (symReady && symCleanup) symCleanup(proc);
    if (dbg) FreeLibrary(dbg);
}

void WriteHeader(char* buf, size_t cap, size_t& pos, const char* kind) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    Append(buf, cap, pos,
           "============================================================\n"
           "%04u-%02u-%02u %02u:%02u:%02u.%03u  %s  PID=%lu TID=%lu\n"
           "%s\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
           kind, GetCurrentProcessId(), GetCurrentThreadId(), g_envLine);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_busy.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    char buf[8192];
    size_t pos = 0;
    WriteHeader(buf, sizeof(buf), pos, "CRASH");
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        char modOff[160];
        ModuleOffset((DWORD64)(uintptr_t)er->ExceptionAddress, modOff, sizeof(modOff));
        Append(buf, sizeof(buf), pos, "Exception: 0x%08lX (%s) at 0x%016llX (%s)\n",
               er->ExceptionCode, ExceptionName(er->ExceptionCode),
               (unsigned long long)(uintptr_t)er->ExceptionAddress, modOff);
        if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            er->NumberParameters >= 2) {
            const char* op = er->ExceptionInformation[0] == 0 ? "reading" :
                             er->ExceptionInformation[0] == 1 ? "writing" :
                             er->ExceptionInformation[0] == 8 ? "executing" : "accessing";
            Append(buf, sizeof(buf), pos, "Detail: %s address 0x%016llX\n",
                   op, (unsigned long long)er->ExceptionInformation[1]);
        }
    }
    Append(buf, sizeof(buf), pos, "Stack:\n");
    WriteStack(buf, sizeof(buf), pos, ep ? ep->ContextRecord : nullptr);
    Append(buf, sizeof(buf), pos, "\n");
    WriteCrashFile(buf, pos);
    if (g_prevFilter && g_prevFilter != UnhandledFilter) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void __cdecl AbortHandler(int) {
    if (g_busy.exchange(true)) return;
    char buf[8192];
    size_t pos = 0;
    WriteHeader(buf, sizeof(buf), pos, "ABORT (SIGABRT)");
    Append(buf, sizeof(buf), pos, "Stack:\n");
    WriteStack(buf, sizeof(buf), pos, nullptr);
    Append(buf, sizeof(buf), pos, "\n");
    WriteCrashFile(buf, pos);
}

}  // namespace

void Install(const std::wstring& logDir, const wchar_t* version, const std::wstring& chromium, bool incognito) {
    if (!g_inited) {
        wcsncpy_s(g_dir, logDir.c_str(), _TRUNCATE);
        _snwprintf_s(g_logPath, _TRUNCATE, L"%s\\crash.log", g_dir);
        _snwprintf_s(g_prevPath, _TRUNCATE, L"%s\\crash.prev.log", g_dir);

        char ver[64]{}, cr[64]{};
        WideCharToMultiByte(CP_UTF8, 0, version ? version : L"", -1, ver, sizeof(ver), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, chromium.c_str(), -1, cr, sizeof(cr), nullptr, nullptr);

        DWORD osMaj = 0, osMin = 0, osBld = 0;
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            typedef LONG(WINAPI* FnRtlGetVersion)(PRTL_OSVERSIONINFOW);
            if (auto fn = (FnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion")) {
                RTL_OSVERSIONINFOW vi{ sizeof(vi) };
                if (fn(&vi) == 0) { osMaj = vi.dwMajorVersion; osMin = vi.dwMinorVersion; osBld = vi.dwBuildNumber; }
            }
        }
        snprintf(g_envLine, sizeof(g_envLine), "NeonNet %s | Chromium %s | Windows %lu.%lu.%lu%s",
                 ver, cr, osMaj, osMin, osBld, incognito ? " | Incognito" : "");
        signal(SIGABRT, AbortHandler);
        g_inited = true;
    }
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(UnhandledFilter);
    if (cur && cur != UnhandledFilter) g_prevFilter = cur;
}

void LogSubprocess(const wchar_t* kind, int status, int code) {
    if (!g_inited) return;
    char buf[1024], k[64]{};
    WideCharToMultiByte(CP_UTF8, 0, kind ? kind : L"subprocess", -1, k, sizeof(k), nullptr, nullptr);
    size_t pos = 0;
    WriteHeader(buf, sizeof(buf), pos, "SUBPROCESS TERMINATED");
    Append(buf, sizeof(buf), pos, "Kind: %s | Status: %d | ExitCode: %d\n\n", k, status, code);
    WriteCrashFile(buf, pos);
}

}  // namespace crash
