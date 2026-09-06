/* 单进程/系统计数器 */
#include "memtool.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <string.h>
#include "counters.h"
#include "snapshot.h"
#include "runtime.h"

int counters_system(sys_mem *sys) { return snapshot_system(sys); }

mt_err counters_process(uint32_t pid, proc_snap *out) {
    memset(out, 0, sizeof *out);
    out->pid = pid;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) return MT_ERR_NOT_FOUND;
        return MT_ERR_PERMISSION;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    if (GetProcessMemoryInfo(h, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof pmc)) {
        out->private_bytes = pmc.PrivateUsage;
        out->working_set = pmc.WorkingSetSize;
        out->peak_working_set = pmc.PeakWorkingSetSize;
        out->page_file_usage = pmc.PagefileUsage;
        out->page_faults = (uint64_t)pmc.PageFaultCount;
    }
    FILETIME kt, ut, ct, et;
    if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        ULARGE_INTEGER a, b;
        a.LowPart=kt.dwLowDateTime; a.HighPart=kt.dwHighDateTime;
        b.LowPart=ut.dwLowDateTime; b.HighPart=ut.dwHighDateTime;
        out->kernel_ms = a.QuadPart/10000; out->user_ms = b.QuadPart/10000;
    }
    CloseHandle(h);

    /* 名称/父进程/线程数 */
    HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (ts != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
        if (Process32FirstW(ts, &pe)) {
            do {
                if (pe.th32ProcessID == pid) {
                    out->ppid = pe.th32ParentProcessID;
                    out->thread_count = pe.cntThreads;
                    int start = 0;
                    for (int j = 0; pe.szExeFile[j]; j++) if (pe.szExeFile[j] == L'\\') start = j + 1;
                    int m = 0;
                    for (int j = start; pe.szExeFile[j] && m < 95; j++) out->name[m++] = (char)pe.szExeFile[j];
                    out->name[m] = '\0';
                    out->rt = runtime_identify(out->name);
                    break;
                }
            } while (Process32NextW(ts, &pe));
        }
        CloseHandle(ts);
    } else {
        snprintf(out->name, sizeof out->name, "pid%u", pid);
    }
    return MT_OK;
}