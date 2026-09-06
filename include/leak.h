#ifndef LEAK_H
#define LEAK_H
#include "sampler.h"

/* 泄漏候选：按时间序列线性回归识别持续增长 */
typedef struct leak_item {
    uint32_t pid;
    char name[96];
    int  kind;           /* 0=private_bytes, 1=handle_count */
    double slope_kb_per_min;
    double r2;           /* 拟合优度 */
    double last_mb;
    double last_handles;
    double cpu_pct;      /* 基于 series 的估算 CPU */
    int samples;
    int severity;        /* 0..5 */
    double time_span_sec;
} leak_item;

/* 返回候选数（<=cap），按严重度降序 */
int leak_detect(sampler *s, leak_item *out, int cap);

#endif