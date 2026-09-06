/* strbuf / 时间辅助 —— 被多模块共用 */
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "memtool.h"

void sb_init(strbuf *s, size_t cap) {
    if (cap < 64) cap = 64;
    s->buf = (char*)malloc(cap);
    s->cap = cap;
    s->len = 0;
    s->oom = !s->buf;
    if (s->buf) s->buf[0] = '\0';
}

void sb_free(strbuf *s) { if (s->buf) { free(s->buf); s->buf = NULL; } s->len = s->cap = 0; }

static int sb_grow(strbuf *s, size_t need) {
    if (s->oom) return 0;
    if (s->len + need + 1 <= s->cap) return 1;
    size_t nc = s->cap ? s->cap : 64;
    while (nc < s->len + need + 1) nc *= 2;
    char *nb = (char*)realloc(s->buf, nc);
    if (!nb) { s->oom = 1; return 0; }
    s->buf = nb; s->cap = nc;
    return 1;
}

void sb_putc(strbuf *s, int c) {
    if (!sb_grow(s, 1)) return;
    s->buf[s->len++] = (char)c;
}
void sb_putn(strbuf *s, const char *v, size_t n) {
    if (!sb_grow(s, n)) return;
    memcpy(s->buf + s->len, v, n);
    s->len += n;
}
void sb_puts(strbuf *s, const char *v) {
    if (!v) return;
    size_t n = strlen(v);
    if (!sb_grow(s, n)) return;
    memcpy(s->buf + s->len, v, n);
    s->len += n;
}
const char *sb_cstr(strbuf *s) {
    if (!s->buf) { s->buf = (char*)calloc(1, 1); s->oom = 1; }
    if (s->buf) s->buf[s->len] = '\0';
    return s->buf;
}
void sb_printf(strbuf *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { sb_puts(s, tmp); return; }
    /* 长内容：分块 */
    char *big = (char*)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_puts(s, big);
    free(big);
}

uint64_t now_ms(void) { return GetTickCount64(); }
uint64_t wall_ms(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (uint64_t)(u.QuadPart / 10000); /* 1601 epoch ms */
}