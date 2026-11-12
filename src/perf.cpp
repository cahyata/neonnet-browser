#include "perf.h"

#include <tlhelp32.h>
#include <psapi.h>
#include <set>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace perf {
namespace {

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10): only declared by newer headers or targets, so spelled out here.
struct MemCountersEx2 {
    DWORD cb; DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
           QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage,
           PrivateUsage, PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

// This process and all processes below it, from one snapshot of the process list.
std::vector<DWORD> Family() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    std::map<DWORD, std::vector<DWORD>> children;
    PROCESSENTRY32W e{ sizeof(e) };
    if (Process32FirstW(snap, &e)) {
        do { if (e.th32ProcessID != e.th32ParentProcessID) children[e.th32ParentProcessID].push_back(e.th32ProcessID); }
        while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    std::vector<DWORD> todo{ GetCurrentProcessId() };
    std::set<DWORD> seen;
    while (!todo.empty()) {
        DWORD p = todo.back(); todo.pop_back();
        if (!seen.insert(p).second) continue;
        out.push_back(p);
        for (DWORD c : children[p]) todo.push_back(c);
    }
    return out;
}

unsigned long long ToU64(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }

std::map<DWORD, unsigned long long> g_lastCpu;   // pid -> kernel+user time (100 ns) at the last sample
unsigned long long g_lastWall = 0;               // wall clock (100 ns) at the last sample

}  // namespace

Snapshot Sample() {
    Snapshot s;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    unsigned long long wall = ToU64(now);
    SYSTEM_INFO si; GetSystemInfo(&si);
    double span = g_lastWall && wall > g_lastWall ? (double)(wall - g_lastWall) * (si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1) : 0;
    std::map<DWORD, unsigned long long> cpuNow;

    for (DWORD pid : Family()) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        ProcInfo pi;
        MemCountersEx2 m{}; m.cb = sizeof(m);
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) pi.bytes = m.PrivateWorkingSetSize;
        else { PROCESS_MEMORY_COUNTERS c{}; if (GetProcessMemoryInfo(h, &c, sizeof(c))) pi.bytes = c.WorkingSetSize; }
        FILETIME cr, ex, kr, us;
        if (GetProcessTimes(h, &cr, &ex, &kr, &us)) {
            unsigned long long t = ToU64(kr) + ToU64(us);
            cpuNow[pid] = t;
            auto it = g_lastCpu.find(pid);
            if (span > 0 && it != g_lastCpu.end() && t >= it->second) pi.cpu = (double)(t - it->second) / span * 100.0;
        }
        CloseHandle(h);
        s.totalBytes += pi.bytes;
        s.totalCpu += pi.cpu;
        s.byPid[pid] = pi;
    }
    g_lastCpu = std::move(cpuNow);
    g_lastWall = wall;
    return s;
}

int TrimWorkingSets() {
    int n = 0;
    for (DWORD pid : Family()) {
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        if (EmptyWorkingSet(h)) n++;
        CloseHandle(h);
    }
    return n;
}

bool SetBackground(DWORD pid, bool low) {
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    SetPriorityClass(h, low ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = low ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof(st));
    CloseHandle(h);
    return true;
}

}  // namespace perf
