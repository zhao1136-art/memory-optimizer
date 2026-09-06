/* 采样线程 + 时间序列环形缓冲
 * 单一轻量线程按 interval_ms 采样全部进程；数据结构有界，工具自身内存低。 */
#include <stdlib.h>
#include <string.h>
#include "sampler.h"
#include "snapshot.h"

static void series_free(sseries *se) {
    if (!se) return;
    free(se->ts); free(se->priv); free(se->ws);
    free(se->handles); free(se->faults); free(se->cpu);
    free(se);
}

static sseries *series_new(uint32_t pid, uint32_t cap) {
    sseries *se = (sseries*)calloc(1, sizeof(sseries));
    if (!se) return NULL;
    se->pid = pid; se->cap = cap;
    se->ts = (double*)calloc(cap, sizeof(double));
    se->priv = (double*)calloc(cap, sizeof(double));
    se->ws = (double*)calloc(cap, sizeof(double));
    se->handles = (double*)calloc(cap, sizeof(double));
    se->faults = (double*)calloc(cap, sizeof(double));
    se->cpu = (double*)calloc(cap, sizeof(double));
    if (!se->ts || !se->priv || !se->ws || !se->handles || !se->faults || !se->cpu) {
        series_free(se); return NULL;
    }
    return se;
}

static void series_push(sseries *se, uint64_t t, double priv, double ws,
                        double handles, double faults, double cpu) {
    uint32_t i = se->head;
    se->ts[i] = (double)t;
    se->priv[i] = priv;
    se->ws[i] = ws;
    se->handles[i] = handles;
    se->faults[i] = faults;
    se->cpu[i] = cpu;
    se->head = (se->head + 1) % se->cap;
    if (se->len < se->cap) se->len++;
}

int series_export(const sseries *se, double *ts, double *priv, double *ws,
                  double *cpu, int cap) {
    int n = (int)se->len;
    if ((int)se->cap > 4096) n = 0;   /* 理论保护：cap 上限 100000，仅导出末尾 */
    if (n <= 0) return 0;
    /* 为支持大窗口，仅从末尾取 min(n,cap) 点 */
    if (n > cap) n = cap;
    uint32_t first_idx = (se->head + se->cap - se->len) % se->cap;
    /* 跳过最旧的 len-cap 个 */
    uint32_t skip = se->len > (uint32_t)cap ? (uint32_t)(se->len - cap) : 0;
    uint32_t start = (first_idx + skip) % se->cap;
    int k = 0;
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        uint32_t idx = (start + i) % se->cap;
        if (ts) ts[k] = se->ts[idx];
        if (priv) priv[k] = se->priv[idx];
        if (ws) ws[k] = se->ws[idx];
        if (cpu) cpu[k] = se->cpu[idx];
        k++;
    }
    return k;
}

sseries *sampler_find_series(sampler *s, uint32_t pid) {
    for (int i = 0; i < s->nseries; i++)
        if (s->series[i]->pid == pid) return s->series[i];
    return NULL;
}

sampler *g_sampler_global = NULL; /* 供部分模块便捷访问 */

static void sampler_update(sampler *s) {
    proc_snap *arr = NULL;
    sys_mem sys;
    int n = snapshot_procs(&arr, &sys);
    uint64_t wall = wall_ms();

    s->sys = sys;
    if (s->sys_series) series_push(s->sys_series, wall, (double)sys.phys_avail,
                                   (double)sys.commit_used, (double)sys.cache_bytes,
                                   (double)sys.handle_count, 0.0);

    /* 替换最新 procs */
    if (arr && n >= 0) {
        if (n > s->procs_cap) {
            proc_snap *nb = (proc_snap*)realloc(s->procs, (size_t)n * sizeof(proc_snap));
            if (nb) { s->procs = nb; s->procs_cap = n; }
        }
        if (s->procs_cap >= n) {
            memcpy(s->procs, arr, (size_t)n * sizeof(proc_snap));
            s->nprocs = n;
        }
    }
    free(arr);

    /* 更新每进程 series，按当前 ws 选择保留集合（有界） */
    /* 先用新快照推进每个的 series */
    for (int i = 0; i < s->nprocs; i++) {
        proc_snap *p = &s->procs[i];
        sseries *se = sampler_find_series(s, p->pid);
        if (!se) {
            if (s->nseries >= s->max_series) continue; /* 达到硬上限，跳过新进程 */
            se = series_new(p->pid, (uint32_t)s->cfg.window_size);
            if (!se) continue;
            se->cap = (uint32_t)s->cfg.window_size;
            se->head = 0;
            se->len = 0;
            s->series[s->nseries++] = se;
        }
        snprintf(se->name, sizeof se->name, "%s", p->name);
        series_push(se, wall, (double)p->private_bytes, (double)p->working_set,
                    (double)p->handle_count, (double)p->page_faults,
                    (double)(p->kernel_ms + p->user_ms));
    }

    /* 清理：不再存在于当前快照的 series（保留，便于观察已退出进程？仅清理超上限） */
    if (s->nseries > s->max_series) {
        /* 移除不再出现的（简单起见：仅处理 pid 不再存在者） */
        for (int i = s->nseries - 1; i >= 0; i--) {
            if (s->nseries <= s->max_series) break;
            if (sampler_find_series(s, s->series[i]->pid) == s->series[i]) {} /* 存在 */
            /* 判断该 pid 是否仍在 procs */
            int alive = 0;
            for (int j = 0; j < s->nprocs; j++) if (s->procs[j].pid == s->series[i]->pid) { alive = 1; break; }
            if (!alive) {
                series_free(s->series[i]);
                s->series[i] = s->series[s->nseries - 1];
                s->series[s->nseries - 1] = NULL;
                s->nseries--;
            }
        }
    }
}

static DWORD WINAPI sampler_thread_fn(LPVOID arg) {
    sampler *s = (sampler*)arg;
    SetEvent(s->ready_evt);
    while (s->running) {
        EnterCriticalSection(&s->lock);
        sampler_update(s);
        s->sample_count++;
        LeaveCriticalSection(&s->lock);
        Sleep((DWORD)s->cfg.interval_ms);
    }
    return 0;
}

int sampler_start(sampler *s, const memtool_config *cfg) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->max_series = 512;
    InitializeCriticalSection(&s->lock);
    s->ready_evt = CreateEvent(NULL, TRUE, FALSE, NULL);
    s->series_cap = s->max_series;
    s->series = (sseries**)calloc((size_t)s->series_cap, sizeof(sseries*));
    if (!s->series) return -1;
    s->procs_cap = 256;
    s->procs = (proc_snap*)calloc((size_t)s->procs_cap, sizeof(proc_snap));
    if (!s->procs) { free(s->series); s->series = NULL; return -1; }
    s->sys_series = series_new(0, (uint32_t)cfg->window_size);
    if (!s->sys_series) { free(s->series); free(s->procs); s->series=NULL; s->procs=NULL; return -1; }
    s->sys_series->cap = (uint32_t)cfg->window_size;
    s->running = true;
    s->started_ms = now_ms();
    s->started_wall = wall_ms();
    g_sampler_global = s;
    s->thread = CreateThread(NULL, 0, sampler_thread_fn, s, 0, NULL);
    if (!s->thread) { s->running = false; return -1; }
    WaitForSingleObject(s->ready_evt, 5000);
    return 0;
}

void sampler_stop(sampler *s) {
    s->running = false;
    if (s->thread) {
        WaitForSingleObject(s->thread, 3000);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    if (s->ready_evt) { CloseHandle(s->ready_evt); s->ready_evt = NULL; }
    if (g_sampler_global == s) g_sampler_global = NULL;
}

void sampler_lock(sampler *s) { EnterCriticalSection(&s->lock); }
void sampler_unlock(sampler *s) { LeaveCriticalSection(&s->lock); }

int sampler_get_procs(sampler *s, proc_snap *out, int cap) {
    int n = s->nprocs;
    if (n > cap) n = cap;
    memcpy(out, s->procs, (size_t)n * sizeof(proc_snap));
    return n;
}
int sampler_get_sys(sampler *s, sys_mem *out) { *out = s->sys; return 0; }