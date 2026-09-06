/* 配置加载：memtool.conf */
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "memtool.h"

void config_defaults(memtool_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->host, sizeof cfg->host, "127.0.0.1");
    cfg->port = 8080;
    cfg->api_token[0] = '\0';
    cfg->allow_remote = 0;
    cfg->cors_origin[0] = '\0';
    cfg->interval_ms = 2000;
    cfg->window_size = 600;
    cfg->storage_enabled = 1;
    snprintf(cfg->data_dir, sizeof cfg->data_dir, "data");
    cfg->retention_hours = 24;
    cfg->auto_watch_top = 40;
    snprintf(cfg->pdf_font, sizeof cfg->pdf_font, "C:\\Windows\\Fonts\\simhei.ttf");
}

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    char *src = s;
    while (*src && isspace((unsigned char)*src)) src++;
    if (src != s) memmove(s, src, strlen(src) + 1);
    /* 原地前移 */
}

static void set_str(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v); }

mt_err config_load(memtool_config *cfg, const char *path) {
    config_defaults(cfg);
    if (!path || !*path) return MT_OK;
    FILE *f = fopen(path, "r");
    if (!f) return MT_OK; /* 无配置文件则用默认 */
    char line[512];
    char section[64] = "";
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '\r') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = '\0'; snprintf(section, sizeof section, "%s", p + 1); }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p; char *val = eq + 1;
        trim(key); trim(val);
        if (strcmp(section, "server") == 0) {
            if (!strcmp(key, "host")) set_str(cfg->host, sizeof cfg->host, val);
            else if (!strcmp(key, "port")) cfg->port = atoi(val);
            else if (!strcmp(key, "api_token")) set_str(cfg->api_token, sizeof cfg->api_token, val);
            else if (!strcmp(key, "allow_remote")) cfg->allow_remote = atoi(val);
            else if (!strcmp(key, "cors_origin")) set_str(cfg->cors_origin, sizeof cfg->cors_origin, val);
        } else if (strcmp(section, "sampler") == 0) {
            if (!strcmp(key, "interval_ms")) cfg->interval_ms = atoi(val);
            else if (!strcmp(key, "window_size")) cfg->window_size = atoi(val);
            else if (!strcmp(key, "auto_watch_top")) cfg->auto_watch_top = atoi(val);
        } else if (strcmp(section, "storage") == 0) {
            if (!strcmp(key, "enabled")) cfg->storage_enabled = atoi(val);
            else if (!strcmp(key, "data_dir")) set_str(cfg->data_dir, sizeof cfg->data_dir, val);
            else if (!strcmp(key, "retention_hours")) cfg->retention_hours = atoi(val);
        } else if (strcmp(section, "report") == 0) {
            if (!strcmp(key, "pdf_font")) set_str(cfg->pdf_font, sizeof cfg->pdf_font, val);
        }
    }
    fclose(f);
    if (cfg->interval_ms < 200) cfg->interval_ms = 200;
    if (cfg->window_size < 2) cfg->window_size = 2;
    if (cfg->window_size > 100000) cfg->window_size = 100000;
    return MT_OK;
}

void config_print(const memtool_config *cfg, FILE *out) {
    fprintf(out, "host=%s port=%d allow_remote=%d token=%s\n"
                 "interval_ms=%d window=%d data_dir=%s retention_h=%d auto_watch_top=%d\n",
            cfg->host, cfg->port, cfg->allow_remote,
            cfg->api_token[0] ? "(set)" : "(none)",
            cfg->interval_ms, cfg->window_size,
            cfg->data_dir, cfg->retention_hours, cfg->auto_watch_top);
    if (cfg->pdf_font[0]) fprintf(out, "pdf_font=%s\n", cfg->pdf_font);
}