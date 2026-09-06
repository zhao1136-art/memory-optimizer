#ifndef RUNTIME_H
#define RUNTIME_H
#include "memtool.h"

/* 按可执行文件名识别运行时类型；失败返回 RT_UNKNOWN */
rt_kind runtime_identify(const char *name);
const char *runtime_label(rt_kind rt);

#endif