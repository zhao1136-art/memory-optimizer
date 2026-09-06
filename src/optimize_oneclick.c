/* 一键优化：智能回收 + 关闭僵尸进程 + 清理系统缓存（纯 C 独立模块） */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "optimize.h"
#include "sampler.h"
#include "protect.h"

#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

/* 近窗 CPU 占用（%）。series 数据不足(<3点)返回 -1，表示无法判定。 */
static double oc_cpu_pct(sampler *s, uint32_t pid) {
    sseries *se = sampler_find_series(s, pid);
    if (!se || se->len < 3) return -1.0;
    uint32_t fi = (se->head + se->cap - se->len) % se->cap;
    uint32_t li = (uint32_t)((se->head + se->cap - 1) % se->cap);
    double dt = (double)(se->ts[li] - se->ts[fi]) / 1000.0;
    if (dt <= 0) return -1.0;
    double p = (se->cpu[li] - se->cpu[fi]) / dt * 100.0;
    return p < 0 ? 0 : p;
}

/* 清理系统 standby 缓存：NtSetSystemInformation(SystemMemoryListInformation, purge standby)。
 * 需管理员权限（SeProfileSingleProcessPrivilege），失败时在 err 写入原因。 */
static void oc_purge_standby(char *err, size_t err_sz) {
    err[0] = '\0';
    HANDLE hTok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        snprintf(err, err_sz, "清缓存失败：无法打开进程令牌（错误 %lu）", (unsigned long)GetLastError());
        return;
    }
    TOKEN_PRIVILEGES tp; memset(&tp, 0, sizeof tp);
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueA(NULL, "SeProfileSingleProcessPrivilege", &tp.Privileges[0].Luid)) {
        snprintf(err, err_sz, "清缓存失败：无法查询特权（错误 %lu）", (unsigned long)GetLastError());
        CloseHandle(hTok); return;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hTok, FALSE, &tp, 0, NULL, NULL);
    DWORD ae = GetLastError();
    CloseHandle(hTok);
    if (ae != ERROR_SUCCESS) {
        snprintf(err, err_sz, "清缓存失败：启用特权失败（错误 %lu，需以管理员运行）", (unsigned long)ae);
        return;
    }
    typedef NTSTATUS (NTAPI *pNtSetSysInfo)(ULONG, PVOID, ULONG);
    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    if (!ntdll) { snprintf(err, err_sz, "清缓存失败：无法加载 ntdll.dll"); return; }
    pNtSetSysInfo fn = (pNtSetSysInfo)(void *)GetProcAddress(ntdll, "NtSetSystemInformation");
    if (!fn) { snprintf(err, err_sz, "清缓存失败：无法获取 NtSetSystemInformation"); FreeLibrary(ntdll); return; }
    ULONG_PTR cmd = 2;                       /* MemoryPurgeStandbyList */
    NTSTATUS st = fn(0x50, &cmd, sizeof cmd);/* SystemMemoryListInformation = 0x50 */
    FreeLibrary(ntdll);
    if (st != 0) snprintf(err, err_sz, "清缓存失败（NTSTATUS 0x%08lx，需管理员权限）", (unsigned long)st);
}

mt_err optimize_oneclick(sampler *s, const protect_list *pl, oneclick_result *out, unsigned options) {
    memset(out, 0, sizeof *out);
    if (options == 0) options = 7u;           /* 默认全部执行 */
    int do_reclaim = (options & 1u) ? 1 : 0;
    int do_kill    = (options & 2u) ? 1 : 0;
    int do_cache   = (options & 4u) ? 1 : 0;

    strbuf sb; sb_init(&sb, 1024);
    uint32_t self_pid = GetCurrentProcessId();

    sampler_lock(s);
    for (int i = 0; i < s->nprocs; i++) {
        proc_snap *p = &s->procs[i];
        if (p->pid == self_pid) continue;                  /* 不处理自身 */
        double ws = (double)p->working_set / 1048576.0;
        if (ws < 64.0) continue;                           /* 小进程不处理 */
        int prot = prot_is_protected(pl, p->pid, p->name);
        double cpu = oc_cpu_pct(s, p->pid);

        /* 1) 关闭僵尸进程：长时间低 CPU(空闲) 且高驻留物理内存 */
        if (do_kill && !prot && cpu >= 0 && cpu < 1.0 && ws >= 64.0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p->pid);
            if (h) {
                if (TerminateProcess(h, 0)) {
                    if (out->killed < 16) {
                        killed_item *ki = &out->killed_list[out->killed];
                        ki->pid = p->pid;
                        snprintf(ki->name, sizeof ki->name, "%s", p->name);
                    }
                    out->killed++;
                    sb_printf(&sb, "[关闭僵尸] %s (pid=%u, 工作集 %.0fMB, CPU %.1f%%)\n",
                              p->name, p->pid, ws, cpu);
                }
                CloseHandle(h);
            } else {
                out->skipped_perm++;
            }
        }
        /* 2) 智能回收：高驻留工作集 -> 修剪到最小允许值 */
        if (do_reclaim && !prot && ws >= 128.0) {
            HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, p->pid);
            if (h) {
                if (SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1)) {
                    out->reclaimed++;
                    out->reclaimed_mb += ws;
                    sb_printf(&sb, "[回收工作集] %s (pid=%u, 释放约 %.0fMB)\n", p->name, p->pid, ws);
                }
                CloseHandle(h);
            } else {
                if (prot) out->skipped_protected++;
                else out->skipped_perm++;
            }
        }
        if (prot) out->skipped_protected++;
    }
    sampler_unlock(s);

    /* 3) 清理系统 standby 缓存 */
    if (do_cache) {
        char cerr[160];
        oc_purge_standby(cerr, sizeof cerr);
        if (cerr[0]) {
            snprintf(out->cache_error, sizeof out->cache_error, "%s", cerr);
            sb_printf(&sb, "[缓存] %s\n", cerr);
        } else {
            out->cache_purged = 1;
            sb_puts(&sb, "[缓存] 系统 standby 缓存已清理\n");
        }
    }

    if (sb.len == 0) sb_puts(&sb, "未执行任何操作（可能没有符合条件的进程）\n");
    snprintf(out->detail, sizeof out->detail, "%s", sb_cstr(&sb));
    sb_free(&sb);
    return MT_OK;
}
