/* 轻量 JSON 序列化器（栈式，自动处理逗号与引号转义）*/
#include <string.h>
#include <ctype.h>
#include "json.h"

void json_init(json *j, size_t cap) {
    sb_init(&j->s, cap);
    j->depth = 0;
}

void json_dtor(json *j) { sb_free(&j->s); j->depth = 0; }

const char *json_cstr(json *j) { return sb_cstr(&j->s); }
size_t json_len(const json *j) { return j->s.len; }

static void j_sep(json *j) {
    int d = j->depth - 1;
    if (d < 0) return;
    /* 刚写过 key：key 已负责逗号，值直接跳过 */
    if (j->keyd[d]) { j->keyd[d] = 0; return; }
    if (j->hasval[d]) sb_putc(&j->s, ',');
    j->hasval[d] = 1;
}

void j_begin_obj(json *j) {
    j_sep(j);
    if (j->depth < 63) {
        j->beg[j->depth] = 1;
        j->hasval[j->depth] = 0;
        j->keyd[j->depth] = 0;
        j->depth++;
    }
    sb_putc(&j->s, '{');
}
void j_begin_arr(json *j) {
    j_sep(j);
    if (j->depth < 63) {
        j->beg[j->depth] = 0;
        j->hasval[j->depth] = 0;
        j->keyd[j->depth] = 0;
        j->depth++;
    }
    sb_putc(&j->s, '[');
}
void j_end(json *j) {
    if (j->depth <= 0) return;
    j->depth--;
    sb_putc(&j->s, j->beg[j->depth] ? '}' : ']');
    if (j->depth > 0) { j->hasval[j->depth - 1] = 1; j->keyd[j->depth - 1] = 0; }
}
void j_key(json *j, const char *k) {
    int d = j->depth - 1;
    j_sep(j);
    if (d >= 0) { j->hasval[d] = 1; j->keyd[d] = 1; }
    sb_putc(&j->s, '"');
    for (const unsigned char *p = (const unsigned char*)k; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { sb_putc(&j->s, '\\'); sb_putc(&j->s, c); }
        else if (c < 0x20) { sb_printf(&j->s, "\\u%04x", c); }
        else sb_putc(&j->s, c);
    }
    sb_puts(&j->s, "\":");
}

void j_strn(json *j, const char *v, size_t n) {
    j_sep(j);
    sb_putc(&j->s, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c == '"' || c == '\\') { sb_putc(&j->s, '\\'); sb_putc(&j->s, c); }
        else if (c < 0x20) sb_printf(&j->s, "\\u%04x", c);
        else sb_putc(&j->s, c);
    }
    sb_putc(&j->s, '"');
}
void j_str(json *j, const char *v) { j_strn(j, v, v ? strlen(v) : 0); }
void j_uint(json *j, uint64_t v) { j_sep(j); sb_printf(&j->s, "%llu", (unsigned long long)v); }
void j_int(json *j, int64_t v) { j_sep(j); sb_printf(&j->s, "%lld", (long long)v); }
void j_bool(json *j, int v) { j_sep(j); sb_puts(&j->s, v ? "true" : "false"); }
void j_null(json *j) { j_sep(j); sb_puts(&j->s, "null"); }
void j_dbl(json *j, double v) {
    j_sep(j);
    if (v != v) { sb_puts(&j->s, "null"); return; }
    char tmp[64];
    if (v == (double)(long long)v && v == (long long)v)
        snprintf(tmp, sizeof tmp, "%.1f", v);
    else
        snprintf(tmp, sizeof tmp, "%.3f", v);
    /* 去尾随 0 与 .0 干扰由调用方决定；此处保留固定精度 */
    sb_puts(&j->s, tmp);
}

/* --- 极简解析 --- */
static const char *js_skip_ws(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static const char *js_find_key(const char *js, const char *key) {
    if (!js || !key) return NULL;      /* 容错：空 body / 空 key */
    size_t kl = strlen(key);
    const char *p = js;
    while (*p) {
        p = js_skip_ws(p);
        if (*p != '"') { p++; continue; }
        const char *ks = p + 1;
        const char *ke = strchr(ks, '"');
        if (!ke) return NULL;
        if ((size_t)(ke - ks) == kl && strncmp(ks, key, kl) == 0) {
            p = js_skip_ws(ke + 1);
            if (*p == ':') return js_skip_ws(p + 1);
        }
        p = ke + 1;
    }
    return NULL;
}

int json_get_int(const char *js, const char *key, int64_t *out, int *found) {
    const char *p = js_find_key(js, key);
    if (!p) { if (found) *found = 0; return -1; }
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) { if (found) *found = 0; return -1; }
    *out = (int64_t)v; if (found) *found = 1; return 0;
}

int json_get_str(const char *js, const char *key, char *out, size_t cap, int *found) {
    const char *p = js_find_key(js, key);
    if (!p || *p != '"') { if (found) *found = 0; return -1; }
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; }
        out[o++] = *p; p++;
    }
    out[o] = '\0';
    if (found) *found = 1;
    return 0;
}