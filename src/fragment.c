/* 内存碎片统计：遍历目标进程虚拟地址空间，统计已提交区分布。
 * 采用受文档支持的 VirtualQueryEx（PROCESS_QUERY_INFORMATION），
 * 对大多数用户进程可读；受保护进程返回权限不足，优雅降级不崩溃。 */
#include <string.h>
#include "fragment.h"
#include "memtool.h"

int fragment_analyze(uint32_t pid, frag_result *out) {
    memset(out, 0, sizeof *out);
    out->pid = pid;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        DWORD e = GetLastError();
        out->ok = 0;
        if (e == ERROR_INVALID_PARAMETER) snprintf(out->error, sizeof out->error, "进程不存在");
        else snprintf(out->error, sizeof out->error, "权限不足（需要 PROCESS_QUERY_INFORMATION 访问权限，受保护/其他用户进程请以管理员运行）");
        return MT_ERR_PERMISSION;
    }
    MEMORY_BASIC_INFORMATION mbi;
    int count = 0, protected_count = 0;
    uint64_t total_committed = 0, total_reserved = 0;
    uint64_t largest = 0;
    uint64_t covered = 0;
    const uint64_t MAX_ADDR = 0x0000FFFFFFFFFFFFULL; /* 64KB 页对齐上限常见用户空间 */
    uintptr_t addr = 0;
    int guard = 0;
    out->ok = 1;
    while (1) {
        if (++guard > 3000000) break; /* 保护循环 */
        SIZE_T siz = VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof mbi);
        if (siz == 0) break; /* 无效/越界 */
        uint64_t region_end = (uint64_t)addr + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            count++;
            uint64_t sz = (uint64_t)mbi.RegionSize;
            total_committed += sz;
            if (sz > largest) largest = sz;
        } else if (mbi.State == MEM_RESERVE) {
            total_reserved += (uint64_t)mbi.RegionSize;
        }
        if (mbi.RegionSize == 0) break;
        /* 防止地址溢出/回绕 */
        if (region_end <= (uint64_t)addr) break;
        addr = (uintptr_t)region_end;
        covered = (uint64_t)addr;
        if (region_end > MAX_ADDR) break;
    }
    CloseHandle(h);

    out->region_count = count;
    out->total_committed = total_committed;
    out->largest_region = largest;
    out->total_reserved = total_reserved;
    (void)covered; (void)protected_count;

    /* 外部碎片指数：1 - 最大连续块/总已提交 */
    if (total_committed > 0) out->frag_idx = 1.0 - (double)largest / (double)total_committed;
    return MT_OK;
}