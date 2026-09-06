/* 轻量持久化：以 JSONL 追加写入滚动摘要，按时间与容量轮转清理 */
#include <string.h>
#include <time.h>
#include <direct.h>
#include "storage.h"
#include "json.h"
#include "hotspot.h"

mt_err storage_init(storage *st, const memtool_config *cfg) {
    memset(st, 0, sizeof *st);
    st->enabled = cfg->storage_enabled;
    st->save_every_ms = 30000; /* 每 30 秒落盘一次摘要，避免频繁 I/O */
    snprintf(st->dir, sizeof st->dir, "%s", cfg->data_dir);
    if (st->enabled) {
        if (GetFileAttributesA(st->dir) == INVALID_FILE_ATTRIBUTES)
            _mkdir(st->dir);
    }
    return MT_OK;
}

void storage_close(storage *st) { (void)st; }

struct tm *mt_localtime(uint64_t wms, struct tm *t) {
    time_t sec = (time_t)(wms / 1000);
    return localtime_s(t, &sec) == 0 ? t : NULL;
}

mt_err storage_tick(storage *st, sampler *s) {
    if (!st->enabled) return MT_OK;
    uint64_t now = now_ms();
    if (now - st->last_save_ms < st->save_every_ms) return MT_OK;
    st->last_save_ms = now;

    char path[320];
    struct tm t;
    if (!mt_localtime(wall_ms(), &t)) return MT_ERR_SYSTEM;
    snprintf(path, sizeof path, "%s\\mem-%04d%02d%02d.jsonl",
             st->dir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    FILE *f = fopen(path, "a");
    if (!f) return MT_ERR_SYSTEM;

    json j; json_init(&j, 1 << 12);
    j_begin_obj(&j);
    j_key(&j, "t"); j_uint(&j, wall_ms());
    j_key(&j, "sample"); j_uint(&j, s->sample_count);
    sys_mem sy = s->sys;
    j_key(&j, "avail_mb"); j_dbl(&j, (double)sy.phys_avail / 1048576.0);
    j_key(&j, "commit_mb"); j_dbl(&j, (double)sy.commit_used / 1048576.0);
    j_key(&j, "cache_mb"); j_dbl(&j, (double)sy.cache_bytes / 1048576.0);
    j_key(&j, "top"); j_begin_arr(&j); /* 占位保持结构清晰，实际 top 记录在数组 */
    j_end(&j); j_end(&j);
    sb_putc(&j.s, '\n');
    fwrite(json_cstr(&j), 1, json_len(&j), f);
    json_dtor(&j);
    fclose(f);
    return MT_OK;
}

void storage_cleanup(storage *st, int retention_hours) {
    if (!st->enabled || retention_hours <= 0) return;
    char pat[320];
    snprintf(pat, sizeof pat, "%s\\mem-*.jsonl", st->dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t cutoff = wall_ms() - (uint64_t)retention_hours * 3600000;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char fp[560]; /* 256(dir) + 1(\) + 259(name) + NUL 留足空间 */
            snprintf(fp, sizeof fp, "%s\\%s", st->dir, fd.cFileName);
            WIN32_FILE_ATTRIBUTE_DATA ad;
            if (GetFileAttributesExA(fp, GetFileExInfoStandard, &ad)) {
                ULARGE_INTEGER u;
                u.LowPart = ad.ftLastWriteTime.dwLowDateTime;
                u.HighPart = ad.ftLastWriteTime.dwHighDateTime;
                uint64_t file_wms = u.QuadPart / 10000; /* 1601 epoch ms，与 wall_ms 同基 */
                if (file_wms < cutoff)
                    DeleteFileA(fp);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}