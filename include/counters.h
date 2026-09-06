#ifndef COUNTERS_H
#define COUNTERS_H
#include "memtool.h"

/* 获取单进程当前内存/CPU 快照；进程不存在返回 MT_ERR_NOT_FOUND；
 * 权限不足时部分字段为 0。 */
mt_err counters_process(uint32_t pid, proc_snap *out);
/* 系统级统计（封装 snapshot_system） */
int counters_system(sys_mem *sys);

#endif