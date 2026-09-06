/* 内存热点：按指标排序 top 进程 */
#include <string.h>
#include <stdlib.h>
#include "hotspot.h"
#include "runtime.h"

static double proc_cpu_pct(sampler *s, uint32_t pid) {
    sseries *se = sampler_find_series(s, pid);
    if (!se || se->len < 2) return 0;
    uint32_t first_idx = (se->head + se->cap - se->len) % se->cap;
    uint32_t last_idx = (uint32_t)((se->head + se->cap - 1) % se->cap);
    double dc = se->cpu[last_idx] - se->cpu[first_idx];
    double dt = (double)(se->ts[last_idx] - se->ts[first_idx]) / 1000.0;
    if (dt <= 0) return 0;
    double p = dc / dt * 100.0;
    return p < 0 ? 0 : p;
}

static int cmp_private(const void *a, const void *b) {
    const hotspot_item *x = (const hotspot_item*)a, *y = (const hotspot_item*)b;
    if (y->private_mb > x->private_mb) return 1;
    if (y->private_mb < x->private_mb) return -1;
    return 0;
}
static int cmp_ws(const void *a, const void *b) {
    const hotspot_item *x = (const hotspot_item*)a, *y = (const hotspot_item*)b;
    if (y->ws_mb > x->ws_mb) return 1;
    if (y->ws_mb < x->ws_mb) return -1;
    return 0;
}
static int cmp_peak(const void *a, const void *b) {
    const hotspot_item *x = (const hotspot_item*)a, *y = (const hotspot_item*)b;
    if (y->peak_mb > x->peak_mb) return 1;
    if (y->peak_mb < x->peak_mb) return -1;
    return 0;
}

int hotspot_rank(sampler *s, const char *metric, int top, hotspot_item *out, int cap) {
    int n = s->nprocs;
    if (n <= 0) return 0;
    if (n > cap) n = cap;
    double total_priv = 0;
    for (int i = 0; i < s->nprocs; i++) total_priv += (double)s->procs[i].private_bytes;

    hotspot_item *tmp = (hotspot_item*)malloc(sizeof(hotspot_item) * (size_t)n);
    if (!tmp) return 0;
    for (int i = 0; i < n; i++) {
        proc_snap *p = &s->procs[i];
        tmp[i].pid = p->pid;
        snprintf(tmp[i].name, sizeof tmp[i].name, "%s", p->name);
        tmp[i].rt = p->rt;
        tmp[i].private_mb = (double)p->private_bytes / 1048576.0;
        tmp[i].ws_mb = (double)p->working_set / 1048576.0;
        tmp[i].peak_mb = (double)p->peak_working_set / 1048576.0;
        tmp[i].cpu_pct = proc_cpu_pct(s, p->pid);
        tmp[i].threads = p->thread_count;
        tmp[i].handle_count = p->handle_count;
        tmp[i].share = total_priv > 0 ? (double)p->private_bytes / total_priv : 0;
    }
    if (strcmp(metric, "working") == 0) qsort(tmp, (size_t)n, sizeof(hotspot_item), cmp_ws);
    else if (strcmp(metric, "peak") == 0) qsort(tmp, (size_t)n, sizeof(hotspot_item), cmp_peak);
    else qsort(tmp, (size_t)n, sizeof(hotspot_item), cmp_private);

    int m = top < n ? top : n;
    memcpy(out, tmp, sizeof(hotspot_item) * (size_t)m);
    free(tmp);
    return m;
}