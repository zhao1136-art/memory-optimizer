/* 进程/系统内存采集
 * 使用受文档支持的 API：Toolhelp 枚举 + PSAPI(GetProcessMemoryInfo)
 * + GlobalMemoryStatusEx/GetPerformanceInfo，保证健壮性并规避未文档化结构差异。 */
#include "memtool.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <string.h>
#include "snapshot.h"
#include "runtime.h"

/* 系统级统计：GlobalMemoryStatusEx + GetPerformanceInfo(缓存/句柄/线程) */
int snapshot_system(sys_mem *sys) {
    memset(sys, 0, sizeof *sys);
    MEMORYSTATUSEX msex;
    msex.dwLength = sizeof msex;
    if (GlobalMemoryStatusEx(&msex)) {
        sys->phys_total = msex.ullTotalPhys;
        sys->phys_avail = msex.ullAvailPhys;
        sys->commit_limit = msex.ullTotalPageFile;
        sys->commit_used = msex.ullTotalPageFile - msex.ullAvailPageFile;
        sys->pagefile_total = msex.ullTotalPageFile;
        sys->pagefile_free = msex.ullAvailPageFile;
    }
    PERFORMANCE_INFORMATION pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        uint64_t page = (uint64_t)pi.PageSize;
        sys->cache_bytes = (uint64_t)pi.SystemCache * page;
        sys->handle_count = pi.HandleCount;
        sys->thread_count = pi.ThreadCount;
    }
    return MT_OK;
}

static void filetime_to_ms(const FILETIME *f, uint64_t *ms) {
    ULARGE_INTEGER u; u.LowPart = f->dwLowDateTime; u.HighPart = f->dwHighDateTime;
    *ms = (uint64_t)(u.QuadPart / 10000); /* 100ns -> ms */
}

int snapshot_procs(proc_snap **out, sys_mem *sys) {
    *out = NULL;
    snapshot_system(sys);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    /* 先计数以分配数组（带上限保护） */
    PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
    int cap = 0;
    if (Process32FirstW(snap, &pe)) { do { cap++; } while (Process32NextW(snap, &pe)); }
    if (cap <= 0) { CloseHandle(snap); return 0; }

    proc_snap *arr = (proc_snap*)calloc((size_t)cap, sizeof(proc_snap));
    if (!arr) { CloseHandle(snap); return -1; }

    pe.dwSize = sizeof pe;
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n >= cap) break;
            proc_snap *p = &arr[n];
            p->pid = pe.th32ProcessID;
            p->ppid = pe.th32ParentProcessID;
            p->thread_count = pe.cntThreads;
            {
                wchar_t w[96];
                int o = 0;
                for (; pe.szExeFile[o] && o < 95; o++) w[o] = pe.szExeFile[o];
                w[o] = L'\0';
                int k = 0;
                for (; w[k]!=L'\\' && w[k]; k++); /* 取 basename 后段 */
                /* 直接取最后反斜杠之后 */
                int start = 0;
                for (int j = 0; w[j]; j++) if (w[j] == L'\\') start = j + 1;
                int m = 0;
                for (int j = start; w[j] && m < 95; j++) p->name[m++] = (char)w[j];
                p->name[m] = '\0';
            }
            p->rt = runtime_identify(p->name);

            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, p->pid);
            if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
            if (h) {
                PROCESS_MEMORY_COUNTERS_EX pmc;
                memset(&pmc, 0, sizeof pmc);
                if (GetProcessMemoryInfo(h, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof pmc)) {
                    p->private_bytes = pmc.PrivateUsage;
                    p->working_set = pmc.WorkingSetSize;
                    p->peak_working_set = pmc.PeakWorkingSetSize;
                    p->page_file_usage = pmc.PagefileUsage;
                    p->page_faults = (uint64_t)pmc.PageFaultCount;
                }
                FILETIME ct, et, kt, ut;
                if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
                    filetime_to_ms(&kt, &p->kernel_ms);
                    filetime_to_ms(&ut, &p->user_ms);
                }
#if 0
                VM_COUNTERS_EX vmc; SIZE_T sz = sizeof vmc;
                if (GetProcessMemoryInfo(h, (PPROCESS_MEMORY_COUNTERS)&vmc, sz)) /* 结构不兼容，禁用 */;
#endif
                CloseHandle(h);
            }
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    *out = arr;
    return n;
}