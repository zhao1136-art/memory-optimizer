#ifndef STORAGE_H
#define STORAGE_H
#include "sampler.h"

typedef struct storage {
    char dir[260];
    uint64_t last_save_ms;
    uint64_t save_every_ms;
    int enabled;
} storage;

mt_err storage_init(storage *st, const memtool_config *cfg);
void storage_close(storage *st);
/* 节流式落盘摘要（每 save_every_ms 一次），由主循环调用 */
mt_err storage_tick(storage *st, sampler *s);
/* 清理超过 retention_hours 的旧文件 */
void storage_cleanup(storage *st, int retention_hours);

#endif