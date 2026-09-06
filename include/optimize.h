#ifndef OPTIMIZE_H
#define OPTIMIZE_H
#include "sampler.h"
#include "protect.h"

typedef enum {
    SUG_RECLAIM = 0,   /* 智能回收工作集 */
    SUG_LARGE_OBJ,     /* 大对象策略 */
    SUG_CACHE,         /* 缓存管理 */
    SUG_LEAK,          /* 泄漏处理 */
    SUG_SYSTEM,        /* 系统级提示 */
} sug_kind;

typedef struct suggest_item {
    int severity;          /* 0..5 */
    sug_kind kind;
    uint32_t pid;
    char name[96];
    int rt;
    char title[96];
    char reason[512];
    int actionable;        /* 能否通过 apply 安全执行 */
    char action[32];       /* 动作标识（如 "reclaim"） */
} suggest_item;

/* 分析生成优化建议，返回条数（<=cap），按严重度降序。 */
int optimize_analyze(sampler *s, suggest_item *out, int cap);
/* 对指定进程执行安全动作：action 目前支持 "reclaim"。受保护进程一律跳过。 */
mt_err optimize_apply(sampler *s, const protect_list *pl, uint32_t pid, const char *action, char *err, size_t err_sz);

/* —— 一键优化 —— */

/* 被关闭的僵尸进程记录（<=16 条） */
typedef struct killed_item {
    uint32_t pid;
    char name[96];
} killed_item;

/* 一键优化执行结果 */
typedef struct oneclick_result {
    int reclaimed;            /* 工作集回收成功的进程数 */
    int killed;               /* 关闭的僵尸进程数 */
    int skipped_protected;    /* 因保护名单/系统进程跳过的进程数 */
    int skipped_perm;         /* 因权限不足跳过的进程数 */
    double reclaimed_mb;      /* 回收动作涉及的驻留内存合计(MB) */
    int cache_purged;         /* 是否成功清理系统 standby 缓存 */
    char cache_error[160];    /* 清缓存失败时的原因 */
    killed_item killed_list[16];
    char detail[4096];        /* 可读的执行明细 */
} oneclick_result;

/*
 * 一键优化：
 * 1) 智能回收：对高驻留且空闲的进程修剪工作集（SetProcessWorkingSetSize）
 * 2) 关闭僵尸进程：对长时间低 CPU 且高驻留的进程 TerminateProcess（带保护名单）
 * 3) 清理系统 standby 缓存（NtSetSystemInformation，需管理员权限，尽力而为）
 * options: bit0=reclaim, bit1=kill_zombie, bit2=purge_cache；0 表示全部执行。
 * pl：受保护名单，命中进程一律跳过（不回收、不关闭）。
 */
mt_err optimize_oneclick(sampler *s, const protect_list *pl, oneclick_result *out, unsigned options);

#endif