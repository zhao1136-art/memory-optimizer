#ifndef PROTECT_H
#define PROTECT_H
#include "memtool.h"

/* 受保护进程名单（用户可增删，持久化到 <data_dir>/protect.txt） */

#define PROTECT_MAX 64

typedef struct protect_list {
    char names[PROTECT_MAX][96]; /* 小写进程名（含 .exe），去重 */
    int  count;
} protect_list;

/* 从 <dir>/protect.txt 加载用户名单；内置系统名单由 prot_is_protected 单独维护 */
void protect_load(protect_list *pl, const char *dir);
/* 保存用户名单到 <dir>/protect.txt */
mt_err protect_save(const protect_list *pl, const char *dir);

/* 用户名单内是否命中（大小写不敏感，精确匹配完整进程名） */
int  protect_contains(const protect_list *pl, const char *name);

/* 增删用户名单（自动转小写/去重/持久化） */
mt_err protect_add(protect_list *pl, const char *dir, const char *name);
mt_err protect_remove(protect_list *pl, const char *dir, const char *name);

/* 统一保护判定：PID<=50 / 内置系统名单 / 用户名单，任一命中即保护 */
int  prot_is_protected(const protect_list *pl, uint32_t pid, const char *name);

/* 判断进程名是否与内置系统名单匹配（不判 PID），供 UI 去重展示 */
int  prot_is_builtin(const char *name);

#endif /* PROTECT_H */