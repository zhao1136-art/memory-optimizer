/* 受保护进程名单：持久化 + 统一判定 */
#include <string.h>
#include <stdio.h>
#include "protect.h"

#define PROTECT_FILE "protect.txt"

/* 内置系统关键/受保护进程名单（小写），始终受保护、不可被用户删除 */
static const char *builtin_protected[] = {
    "[system process]", "system", "registry", "smss.exe", "csrss.exe",
    "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe", "lsm.exe",
    "svchost.exe", "dwm.exe", "fontdrvhost.exe", "sihost.exe", "taskhostw.exe",
    "explorer.exe", "conhost.exe", "memtool.exe"
};
#define N_BUILTIN ((int)(sizeof(builtin_protected) / sizeof(builtin_protected[0])))

static void pl_lower(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        char c = s[i];
        d[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    d[i] = '\0';
}

static int builtin_match(const char *name) {
    if (!name) return 0;
    char lb[96]; pl_lower(lb, name, sizeof lb);
    for (int i = 0; i < N_BUILTIN; i++)
        if (!strcmp(lb, builtin_protected[i])) return 1;
    return 0;
}

int prot_is_builtin(const char *name) { return builtin_match(name); }

int protect_contains(const protect_list *pl, const char *name) {
    if (!pl || !name) return 0;
    char lb[96]; pl_lower(lb, name, sizeof lb);
    for (int i = 0; i < pl->count; i++)
        if (!strcmp(pl->names[i], lb)) return 1;
    return 0;
}

int prot_is_protected(const protect_list *pl, uint32_t pid, const char *name) {
    if (pid <= 50) return 1;               /* 系统关键 PID 段 */
    if (name && builtin_match(name)) return 1;
    if (pl && protect_contains(pl, name)) return 1;
    return 0;
}

void protect_load(protect_list *pl, const char *dir) {
    if (!pl) return;
    pl->count = 0;
    char fp[320];
    snprintf(fp, sizeof fp, "%s\\%s", dir ? dir : ".", PROTECT_FILE);
    FILE *f = fopen(fp, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && pl->count < PROTECT_MAX) {
        char *p = line;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == '#' || *p == '\0') continue;
        char *e = p + strlen(p);
        while (e > p && (*--e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e = '\0';
        if (!*p) continue;
        e = p + strlen(p);
        pl_lower(pl->names[pl->count], p, sizeof pl->names[pl->count]);
        /* 去重 */
        int dup = 0;
        for (int i = 0; i < pl->count; i++)
            if (!strcmp(pl->names[i], pl->names[pl->count])) { dup = 1; break; }
        if (!dup) pl->count++;
    }
    fclose(f);
}

mt_err protect_save(const protect_list *pl, const char *dir) {
    char fp[320];
    snprintf(fp, sizeof fp, "%s\\%s", dir ? dir : ".", PROTECT_FILE);
    FILE *f = fopen(fp, "w");
    if (!f) return MT_ERR_SYSTEM;
    fprintf(f, "# MemOptimizer 受保护进程名单（每行一个进程名，小写，含扩展名）\n");
    for (int i = 0; i < (pl ? pl->count : 0); i++)
        fprintf(f, "%s\n", pl->names[i]);
    if (fclose(f) != 0) return MT_ERR_SYSTEM;
    return MT_OK;
}

mt_err protect_add(protect_list *pl, const char *dir, const char *name) {
    if (!pl || !name || !*name) return MT_ERR_PARAM;
    char lb[96]; pl_lower(lb, name, sizeof lb);
    if (!*lb) return MT_ERR_PARAM;
    for (int i = 0; i < pl->count; i++)
        if (!strcmp(pl->names[i], lb)) return MT_OK;   /* 已存在 */
    if (pl->count >= PROTECT_MAX) return MT_ERR_BUSY;
    snprintf(pl->names[pl->count], sizeof pl->names[pl->count], "%s", lb);
    pl->count++;
    return protect_save(pl, dir);
}

mt_err protect_remove(protect_list *pl, const char *dir, const char *name) {
    if (!pl || !name || !*name) return MT_ERR_PARAM;
    char lb[96]; pl_lower(lb, name, sizeof lb);
    int found = 0;
    for (int i = 0; i < pl->count; i++) {
        if (!strcmp(pl->names[i], lb)) {
            pl->names[i][0] = '\0';
            for (int j = i; j + 1 < pl->count; j++)
                memcpy(pl->names[j], pl->names[j + 1], 96);
            pl->count--;
            found = 1;
            break;
        }
    }
    if (!found) return MT_ERR_NOT_FOUND;
    return protect_save(pl, dir);
}