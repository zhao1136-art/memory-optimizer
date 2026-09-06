#ifndef SAMPLER_H
#define SAMPLER_H
#include "memtool.h"

/* 单进程时间序列（定长环形缓冲） */
typedef struct sseries {
    uint32_t pid;
    uint32_t cap;
    uint32_t head;   /* 下一次写入位置 */
    uint32_t len;    /* 已存点数 <= cap */
    char name[96];   /* 最近一次的可执行名 */
    double *ts;      /* ms */
    double *priv;
    double *ws;
    double *handles;
    double *faults;
    double *cpu;     /* 累计 kernel+user ms */
} sseries;

typedef struct sampler {
    memtool_config cfg;
    volatile bool running;
    CRITICAL_SECTION lock;
    HANDLE thread;
    HANDLE ready_evt;

    /* 最近一次全量快照 */
    proc_snap *procs;
    int nprocs;
    int procs_cap;
    sys_mem  sys;

    uint64_t sample_count;
    uint64_t started_ms;
    uint64_t started_wall;

    /* 时间序列 */
    sseries **series;
    int nseries;
    int series_cap;
    int max_series;      /* 硬上限，防止内存无界 */
    sseries *sys_series; /* 系统级时间序列(pid=0) */
} sampler;

int sampler_start(sampler *s, const memtool_config *cfg);
void sampler_stop(sampler *s);

/* 返回最新快照（内部拷贝到调用方提供的缓冲区），线程安全 */
int sampler_get_procs(sampler *s, proc_snap *out, int cap);
int sampler_get_sys(sampler *s, sys_mem *out);
sseries *sampler_find_series(sampler *s, uint32_t pid); /* 需持有锁 */
void sampler_lock(sampler *s);
void sampler_unlock(sampler *s);

/* 从 series 读出定序数据 (ts, priv, ws, cpu) 到调用方数组 */
int series_export(const sseries *se, double *ts, double *priv, double *ws,
                  double *cpu, int cap);

#endif