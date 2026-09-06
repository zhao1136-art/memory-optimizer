/* 优化建议规则引擎 + 安全动作执行 */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "optimize.h"
#include "leak.h"
#include "runtime.h"
#include "protect.h"
#include "counters.h"

static void push(suggest_item *out, int *n, int cap, int sev, sug_kind k,
                 uint32_t pid, const char *name, int rt, int actionable, const char *action,
                 const char *title, const char *reason) {
    if (*n >= cap) return;
    suggest_item *it = &out[*n];
    memset(it, 0, sizeof *it);
    it->severity = sev; it->kind = k; it->pid = pid; it->rt = rt;
    snprintf(it->name, sizeof it->name, "%s", name);
    snprintf(it->title, sizeof it->title, "%s", title);
    snprintf(it->reason, sizeof it->reason, "%s", reason);
    it->actionable = actionable;
    if (actionable && action) snprintf(it->action, sizeof it->action, "%s", action);
    (*n)++;
}

static int cmp_sug(const void *a, const void *b) {
    const suggest_item *x = (const suggest_item*)a, *y = (const suggest_item*)b;
    if (y->severity != x->severity) return y->severity - x->severity;
    return 0;
}

int optimize_analyze(sampler *s, suggest_item *out, int cap) {
    int n = 0;
    if (cap <= 0) return 0;

    /* 1) 智能回收：工作集显著高于私有字节（空闲驻留页多） */
    for (int i = 0; i < s->nprocs; i++) {
        proc_snap *p = &s->procs[i];
        double priv = (double)p->private_bytes, ws = (double)p->working_set;
        if (priv <= 0 || ws <= 0) continue;
        double ratio = ws / priv;
        if (ratio >= 3.0 && ws > 256.0 * 1048576.0) {
            char reason[512];
            snprintf(reason, sizeof reason,
                     "进程工作集 %.0f MB 远超私有字节 %.0f MB（倍数 %.1fx），驻留页可能空闲占用物理内存。"
                     "可执行智能回收（修剪工作集）以释放物理内存。",
                     ws / 1048576.0, priv / 1048576.0, ratio);
            int sev = ratio >= 8.0 ? 4 : (ratio >= 5.0 ? 3 : 2);
            push(out, &n, cap, sev, SUG_RECLAIM, p->pid, p->name, p->rt, 1, "reclaim",
                 "工作集显著高于私有占用，建议回收空闲物理内存", reason);
        }
    }

    /* 2) 大对象策略：峰值/私有极高且提交区大 */
    double total_priv = 0;
    for (int i = 0; i < s->nprocs; i++) total_priv += (double)s->procs[i].private_bytes;
    for (int i = 0; i < s->nprocs; i++) {
        proc_snap *p = &s->procs[i];
        double priv = (double)p->private_bytes;
        if (priv > 1024.0 * 1048576.0 && total_priv > 0 && priv / total_priv > 0.2) {
            char reason[512];
            snprintf(reason, sizeof reason,
                     "私有字节 %.0f MB，占全部进程私有内存的 %.1f%%。峰值工作集 %.0f MB。"
                     "建议审视大块/大对象分配：尽量批量释放、采用对象池或大块连续分配，"
                     "并启用/加大分配保留以降低碎片与峰值.",
                     priv / 1048576.0, priv / total_priv * 100.0,
                     (double)p->peak_working_set / 1048576.0);
            push(out, &n, cap, 3, SUG_LARGE_OBJ, p->pid, p->name, p->rt, 0, "",
                 "存在大内存持有者，建议大对象优化", reason);
        }
    }

    /* 3) 缓存管理：系统可用内存偏低且缓存(standby)大 */
    {
        sys_mem *so = &s->sys;
        double avail = (double)so->phys_avail;
        double total = (double)so->phys_total;
        double cache = (double)so->cache_bytes;
        if (total > 0) {
            double a_pct = avail / total * 100.0;
            double c_pct = cache / total * 100.0;
            if (a_pct < 15.0) {
                char reason[512];
                snprintf(reason, sizeof reason,
                         "系统可用物理内存仅 %.1f%%（%.0f MB），缓存/standby 约 %.1f%%（%.0f MB）。"
                         "建议释放/压缩系统缓存、关闭后台高驻留应用，或增加物理内存。",
                         a_pct, avail / 1048576.0, c_pct, cache / 1048576.0);
                push(out, &n, cap, c_pct > 25.0 ? 4 : 3, SUG_CACHE, 0, "system", RT_UNKNOWN,
                     0, "", "系统可用内存偏低", reason);
            }
        }
    }

    /* 4) 泄漏处理：若检测到泄漏候选给建议 */
    {
        leak_item li[8];
        int lc = leak_detect(s, li, 8);
        for (int i = 0; i < lc; i++) {
            if (li[i].severity <= 0) continue;
            char reason[512];
            if (li[i].kind == 0)
                snprintf(reason, sizeof reason,
                         "私有内存以约 %.0f KB/分钟 持续增长（拟合 R²=%.2f，历时 %.0f 秒），"
                         "疑似内存泄漏。请检查该进程的对象生命周期：及时释放不再使用的引用、"
                         "避免循环引用、限制缓存无界增长。",
                         li[i].slope_kb_per_min, li[i].r2, li[i].time_span_sec);
            else
                snprintf(reason, sizeof reason,
                         "句柄数以 %.0f 个/分钟 持续增长（R²=%.2f），疑似句柄泄漏。"
                         "请确保每次打开的资源（文件、注册表、GDI 对象等）都被关闭。",
                         li[i].slope_kb_per_min * 1024.0 / 60.0, li[i].r2);
            push(out, &n, cap, li[i].severity, SUG_LEAK, li[i].pid, li[i].name, RT_UNKNOWN,
                 0, "", "检测到疑似内存泄漏", reason);
        }
    }

    qsort(out, (size_t)n, sizeof(suggest_item), cmp_sug);
    return n;
}

mt_err optimize_apply(sampler *s, const protect_list *pl, uint32_t pid, const char *action, char *err, size_t err_sz) {
    (void)s;
    if (!action || (strcmp(action, "reclaim") != 0)) {
        if (err) snprintf(err, err_sz, "不支持的 action: %s", action ? action : "(null)");
        return MT_ERR_PARAM;
    }
    /* 受保护进程一律跳过：获取进程名用于名单匹配 */
    proc_snap cur;
    mt_err pe = counters_process(pid, &cur);
    if (pe == MT_ERR_NOT_FOUND) {
        if (err) snprintf(err, err_sz, "进程不存在");
        return MT_ERR_NOT_FOUND;
    }
    if (prot_is_protected(pl, pid, cur.name)) {
        if (err) snprintf(err, err_sz, "该进程受保护，已跳过（受保护进程不执行任何优化动作）");
        return MT_ERR_PERMISSION;
    }
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        DWORD e = GetLastError();
        if (err) {
            if (e == ERROR_INVALID_PARAMETER) snprintf(err, err_sz, "进程不存在");
            else snprintf(err, err_sz, "权限不足，需要 PROCESS_SET_QUOTA（同用户进程或管理员）");
        }
        return e == ERROR_INVALID_PARAMETER ? MT_ERR_NOT_FOUND : MT_ERR_PERMISSION;
    }
    /* -1 表示修剪工作集到最小允许值 */
    BOOL ok = SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1);
    CloseHandle(h);
    if (!ok) {
        if (err) snprintf(err, err_sz, "SetProcessWorkingSetSize 失败，错误码 %lu", (unsigned long)GetLastError());
        return MT_ERR_SYSTEM;
    }
    if (err) snprintf(err, err_sz, "ok");
    return MT_OK;
}