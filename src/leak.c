/* 泄漏检测：对时间序列(私有字节/句柄)做最小二乘线性回归，识别持续单调增长 */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "leak.h"

/* 最小二乘回归：y = slope*x + inter，x 为秒。返回 R²。 */
static double linreg(const double *x, const double *y, int n,
                     double *slope, double *inter) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (int i = 0; i < n; i++) {
        sx += x[i]; sy += y[i];
        sxx += x[i] * x[i]; sxy += x[i] * y[i]; syy += y[i] * y[i];
    }
    double denom = (double)n * sxx - sx * sx;
    if (fabs(denom) < 1e-9) { *slope = 0; *inter = sy / n; return 0.0; }
    *slope = ((double)n * sxy - sx * sy) / denom;
    *inter = (sy - *slope * sx) / (double)n;
    double sct = syy - sy * sy / (double)n;
    double ssr = 0;
    for (int i = 0; i < n; i++) { double e = y[i] - (*inter + *slope * x[i]); ssr += e * e; }
    if (fabs(sct) < 1e-12) return (fabs(sy) < 1e-9) ? 1.0 : 0.0;
    return 1.0 - ssr / sct;
}

static void detect_kind(const sseries *se, int kind, leak_item *o) {
    int cap = (int)se->len;
    if (cap > 2048) cap = 2048;
    if (cap < 12) return;
    double *xs = (double*)malloc(sizeof(double) * cap);
    double *ys = (double*)malloc(sizeof(double) * cap);
    if (!xs || !ys) { free(xs); free(ys); return; }

    /* 按时间序导出（从旧到新） */
    int n = 0;
    uint32_t first_idx = (se->head + se->cap - se->len) % se->cap;
    uint32_t skip = se->len > (uint32_t)cap ? (uint32_t)(se->len - cap) : 0;
    uint32_t start = (first_idx + skip) % se->cap;
    double t0 = se->ts[start];
    double y_base = kind == 0 ? se->priv[start] : se->handles[start];
    for (uint32_t i = 0; i < (uint32_t)cap; i++) {
        uint32_t idx = (start + i) % se->cap;
        xs[n] = (double)(se->ts[idx] - t0) / 1000.0;    /* 秒 */
        ys[n] = kind == 0 ? (double)se->priv[idx] : (double)se->handles[idx];
        n++;
    }
    if (n < 12) { free(xs); free(ys); return; }

    /* 只看相对起点增长 */
    for (int i = 0; i < n; i++) ys[i] -= y_base;

    double slope, inter, r2;
    r2 = linreg(xs, ys, n, &slope, &inter);
    double span = xs[n - 1] - xs[0];
    double cur = ys[n - 1];
    free(xs); free(ys);

    o->samples = n;
    o->time_span_sec = span;
    o->r2 = r2;
    o->last_mb = kind == 0 ? (double)(y_base + cur) / 1048576.0 : 0;
    o->last_handles = kind == 0 ? 0 : (y_base + cur);
    o->cpu_pct = 0;

    int valid = 0;
    if (kind == 0) {
        double rate_kb_min = slope * 60.0 / 1024.0;   /* bytes/sec -> KB/min */
        o->slope_kb_per_min = rate_kb_min;
        /* 需要足够时间跨度 + 正增长 + 拟合较好 */
        if (span >= 10.0 && cur > 0 && r2 >= 0.45 && rate_kb_min >= 8.0) valid = 1;
    } else {
        double rate_min = slope * 60.0;               /* 句柄/min */
        o->slope_kb_per_min = slope / 1024.0 * 60.0;
        if (span >= 10.0 && cur > 2 && r2 >= 0.5 && rate_min >= 6.0) valid = 1;
    }
    if (!valid) { o->severity = 0; o->slope_kb_per_min = 0; return; }

    /* 严重度：私有字节速率 */
    double mb_per_min = o->slope_kb_per_min / 1024.0;
    if (mb_per_min > 5.0) o->severity = 5;
    else if (mb_per_min > 1.0) o->severity = 4;
    else if (mb_per_min > 0.25) o->severity = 3;
    else if (mb_per_min > 0.0625) o->severity = 2;
    else o->severity = 1;

    /* CPU 估算：从 series 首尾 cpu_total 增量 */
    {
        double c0 = se->cpu[start], e0 = se->ts[start];
        uint32_t last_idx = (uint32_t)((uint32_t)n == 0 ? start : (start + ((uint32_t)n - 1)) % se->cap);
        double c1 = se->cpu[last_idx], e1 = se->ts[last_idx];
        double dt = (e1 - e0) / 1000.0;
        if (dt > 0) o->cpu_pct = (c1 - c0) / dt * 100.0;
        if (o->cpu_pct < 0) o->cpu_pct = 0;
    }
}

int leak_detect(sampler *s, leak_item *out, int cap) {
    int n = 0;
    for (int i = 0; i < s->nseries; i++) {
        sseries *se = s->series[i];
        if (!se || se->pid == 0) continue;
        leak_item a, b;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        a.pid = se->pid; b.pid = se->pid;
        snprintf(a.name, sizeof a.name, "%s", se->name);
        snprintf(b.name, sizeof b.name, "%s", se->name);
        a.kind = 0; b.kind = 1;
        detect_kind(se, 0, &a);
        detect_kind(se, 1, &b);
        if (a.severity > 0) { out[n++] = a; }
        else if (b.severity > 0) { out[n++] = b; }
        if (n >= cap) break;
    }
    return n;
}