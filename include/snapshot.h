#ifndef SNAPSHOT_H
#define SNAPSHOT_H
#include "memtool.h"

/* 采样全部进程 + 系统级统计。
 * out: 调用方 free 释放；返回进程数；失败返回 <=0。 */
int snapshot_procs(proc_snap **out, sys_mem *sys);
/* 仅系统级统计 */
int snapshot_system(sys_mem *sys);

#endif