#ifndef HOTSPOT_H
#define HOTSPOT_H
#include "sampler.h"

typedef struct hotspot_item {
    uint32_t pid;
    char name[96];
    int rt;
    double private_mb, ws_mb, peak_mb;
    double cpu_pct;
    uint32_t threads;
    uint64_t handle_count;
    double share;   /* 私有字节占全体比例 0..1 */
} hotspot_item;

/* 按 metric("private"|"working"|"peak") 返回 top 热点；cap 为输出上限。 */
int hotspot_rank(sampler *s, const char *metric, int top, hotspot_item *out, int cap);

#endif