#ifndef FRAGMENT_H
#define FRAGMENT_H
#include <stdint.h>
#include <stdbool.h>

typedef struct frag_result {
    uint32_t pid;
    int ok;                  /* 1=成功,0=权限不足/失败 */
    int region_count;        /* 已提交(commit)区域数 */
    uint64_t total_committed;/* 已提交字节 */
    uint64_t largest_region; /* 最大连续已提交区域 */
    uint64_t total_reserved; /* 保留(reserve)字节 */
    double frag_idx;         /* 外部碎片指数 0(整块)~1(高度碎片化) */
    char error[160];
} frag_result;

/* 对目标进程做提交区碎片分析。无需管理员对大多数同用户进程；受保护进程返回 ok=0。 */
int fragment_analyze(uint32_t pid, frag_result *out);

#endif