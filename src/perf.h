// Process memory and CPU figures for the performance panel. Plain Win32, no CEF: the browser
// process and every process it started (renderers, GPU, network, utility) are looked up with a
// process snapshot, so nothing here depends on how Chromium names or numbers them.
#pragma once
#include <windows.h>
#include <map>

namespace perf {

struct ProcInfo {
    unsigned long long bytes = 0;   // private working set: memory this process alone holds in RAM
    double cpu = 0;                 // percent of the whole CPU (all cores) since the previous Sample()
};

struct Snapshot {
    std::map<DWORD, ProcInfo> byPid;   // this process and all its descendants
    unsigned long long totalBytes = 0;
    double totalCpu = 0;
};

// Measures this process and everything it started. CPU is relative to the previous call, so the
// first value after a long pause covers the whole pause.
Snapshot Sample();

// Asks Windows to take unused pages out of the working set of this process and all its
// descendants (they come back from the page file or from the standby list when used again).
// Returns how many processes were trimmed.
int TrimWorkingSets();

// Background priority for one process: low = below-normal priority and Windows' power-saving
// ("efficiency") mode; !low = back to normal. Returns false if the process could not be opened.
bool SetBackground(DWORD pid, bool low);

}  // namespace perf
