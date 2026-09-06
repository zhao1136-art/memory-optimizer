/* REST 路由分发 + 鉴权 + CORS + 静态资源伺服 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "routes.h"
#include "json.h"
#include "runtime.h"
#include "leak.h"
#include "hotspot.h"
#include "fragment.h"
#include "optimize.h"
#include "report.h"
#include "pdf.h"
#include "counters.h"

/* ---------- 工具函数 ---------- */

static int url_decode(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char*)s; *p && o + 1 < cap; p++) {
        if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
            int hi = isdigit(p[1]) ? p[1] - '0' : tolower(p[1]) - 'a' + 10;
            int lo = isdigit(p[2]) ? p[2] - '0' : tolower(p[2]) - 'a' + 10;
            out[o++] = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') out[o++] = ' ';
        else out[o++] = (char)*p;
    }
    out[o] = '\0';
    return (int)o;
}

static int q_param(const http_req *req, const char *name, char *out, size_t cap) {
    char q[512];
    url_decode(req->query, q, sizeof q);
    char *p = q;
    while (*p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(p, '=');
        if (eq && !strncmp(p, name, (size_t)(eq - p)) && (eq-p) == (int)strlen(name)) {
            url_decode(eq + 1, out, cap);
            return 0;
        }
        if (!amp) break;
        p = amp + 1;
    }
    if (out && cap) out[0] = '\0';
    return -1;
}

static int header_has(const http_req *req, const char *name, char *out, size_t cap) {
    const char *p = req->header;
    size_t nl = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        int pre_ok = p == req->header || *(p - 1) == '\n' || *(p - 1) == '\r';
        if (pre_ok && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = strchr(v, '\r');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l + 1 > cap) l = cap - 1;
            memcpy(out, v, l); out[l] = '\0';
            return 0;
        }
        p += nl;
    }
    if (out && cap) out[0] = '\0';
    return -1;
}

static http_resp jresp(json *pj, int status) {
    http_resp r; memset(&r, 0, sizeof r);
    r.status = status == 0 ? 200 : status;
    r.content_type = "application/json; charset=utf-8";
    size_t len = json_len(pj);
    char *b = (char*)malloc(len + 1);
    if (b) {
        memcpy(b, json_cstr(pj), len + 1);
        r.body = b; r.body_len = len; r.free_body = 1;
    } else { r.status = 500; }
    json_dtor(pj);
    return r;
}

static http_resp json_err(int status, const char *msg) {
    json j; json_init(&j, 256);
    j_begin_obj(&j); j_key(&j, "error"); j_str(&j, msg); j_end(&j);
    return jresp(&j, status);
}

static http_resp text_resp(const char *body, int status, const char *ct, int freeit) {
    http_resp r; memset(&r, 0, sizeof r);
    r.status = status; r.content_type = ct;
    r.body = body; r.body_len = body ? strlen(body) : 0; r.free_body = freeit;
    return r;
}

static void fmt_bytes(double mb, char *out, size_t cap) {
    if (mb >= 1024) snprintf(out, cap, "%.2f GB", mb / 1024.0);
    else snprintf(out, cap, "%.0f MB", mb);
}

/* ---------- 鉴权 / CORS ---------- */
static int check_access(const app_ctx *ctx, const http_req *req, char *orig_out, size_t orig_cap, char *err, size_t err_cap) {
    int is_local = !strcmp(req->peer_ip, "127.0.0.1") || !strcmp(req->peer_ip, "::1") ||
                   !strcmp(req->peer_ip, "0.0.0.0") || !strcmp(req->peer_ip, "localhost");
    /* 读原 Origin */
    char origin[512]; int has_origin = header_has(req, "Origin", origin, sizeof origin) == 0;
    if (orig_out) snprintf(orig_out, orig_cap, "%s", has_origin ? origin : "");

    if (!ctx->cfg.allow_remote && !is_local) {
        snprintf(err, err_cap, "仅允许本机访问。如需远程请设置 allow_remote=1 与 api_token。");
        return 403;
    }
    if (ctx->cfg.allow_remote && !is_local) {
        /* 远程必须携带 Bearer token */
        char auth[260]; int has = header_has(req, "Authorization", auth, sizeof auth) == 0;
        char tok[132]; int ok = 0;
        if (has && !strncmp(auth, "Bearer ", 7)) {
            url_decode(auth + 7, tok, sizeof tok);
            if (!strcmp(tok, ctx->cfg.api_token)) ok = 1;
        }
        if (!ok) { snprintf(err, err_cap, "无效的访问令牌"); return 401; }
    }
    /* CORS：若配置白名单，Origin 必须在名单内 */
    if (has_origin && ctx->cfg.cors_origin[0]) {
        char wl[512]; snprintf(wl, sizeof wl, "%s", ctx->cfg.cors_origin);
        int found = 0;
        char *p = wl;
        while (*p) {
            char *c = strchr(p, ',');
            if (c) *c = '\0';
            while (*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p);
            while (e > p && (*--e==' '||*e=='\t')) *e = '\0';
            if (!strcmp(p, origin)) { found = 1; break; }
            if (!c) break;
            p = c + 1;
        }
        if (!found) { snprintf(err, err_cap, "未在白名单的 Origin"); return 403; }
    }
    return 0;
}

/* ---------- 静态资源 ---------- */
static const char *mime_of(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!_stricmp(ext, ".html") || !_stricmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!_stricmp(ext, ".js")) return "application/javascript; charset=utf-8";
    if (!_stricmp(ext, ".css")) return "text/css; charset=utf-8";
    if (!_stricmp(ext, ".json")) return "application/json; charset=utf-8";
    if (!_stricmp(ext, ".png")) return "image/png";
    if (!_stricmp(ext, ".svg")) return "image/svg+xml";
    if (!_stricmp(ext, ".ico")) return "image/x-icon";
    if (!_stricmp(ext, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

static void web_root_of(char *out, size_t cap) {
    const char *cand[] = { "src\\web", "web", "bin\\web", "." };
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        char t[320]; snprintf(t, sizeof t, "%s\\index.html", cand[i]);
        if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", cand[i]);
            return;
        }
    }
    snprintf(out, cap, ".");
}

static http_resp serve_static(const char *uri, const char *base) {
    char root[320];
    if (!base || !base[0]) web_root_of(root, sizeof root);
    else snprintf(root, sizeof root, "%s", base);
    /* 防目录穿越/注入：必须以 / 开头；拒绝 ..、':'(绝对URL) 及 query/fragment */
    if (uri[0] != '/' || strstr(uri, "..") || strchr(uri, ':') || strchr(uri, '?') || strchr(uri, '#'))
        return json_err(400, "非法路径");
    char rel[512] = "index.html";
    if (strcmp(uri, "/") != 0 && uri[0]) snprintf(rel, sizeof rel, "%s", uri + 1);

    char fp[800];
    snprintf(fp, sizeof fp, "%s\\%s", root, rel);
    FILE *f = fopen(fp, "rb");
    if (!f) return json_err(404, "文件不存在");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return json_err(500, "读取错误"); }
    char *b = (char*)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return json_err(500, "内存不足"); }
    size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[rd] = '\0';
    return text_resp(b, 200, mime_of(rel), 1);
}

/* ---------- API 实现 ---------- */

static void proc_to_json(json *j, const proc_snap *p, double cpu) {
    j_begin_obj(j);
    j_key(j, "pid"); j_uint(j, p->pid);
    j_key(j, "ppid"); j_uint(j, p->ppid);
    j_key(j, "name"); j_str(j, p->name);
    j_key(j, "runtime"); j_str(j, runtime_label((rt_kind)p->rt));
    j_key(j, "private_mb"); j_dbl(j, (double)p->private_bytes / 1048576.0);
    j_key(j, "ws_mb"); j_dbl(j, (double)p->working_set / 1048576.0);
    j_key(j, "peak_mb"); j_dbl(j, (double)p->peak_working_set / 1048576.0);
    j_key(j, "pagefile_mb"); j_dbl(j, (double)p->page_file_usage / 1048576.0);
    j_key(j, "faults"); j_uint(j, p->page_faults);
    j_key(j, "handles"); j_uint(j, p->handle_count);
    j_key(j, "threads"); j_uint(j, p->thread_count);
    j_key(j, "cpu_pct"); j_dbl(j, cpu);
    j_end(j);
}

static double cpu_pct_for(sampler *s, uint32_t pid) {
    sseries *se = sampler_find_series(s, pid);
    if (!se || se->len < 2) return 0;
    uint32_t fi = (se->head + se->cap - se->len) % se->cap;
    uint32_t li = (uint32_t)((se->head + se->cap - 1) % se->cap);
    double dt = (double)(se->ts[li] - se->ts[fi]) / 1000.0;
    if (dt <= 0) return 0;
    double p = (se->cpu[li] - se->cpu[fi]) / dt * 100.0;
    return p < 0 ? 0 : p;
}

static http_resp api_overview(const app_ctx *ctx, const http_req *req) {
    (void)req;
    sampler *s = ctx->sampler;
    json j; json_init(&j, 1 << 10);
    char tmp[64];
    /* 系统 */
    sys_mem sy;
    sampler_lock(s); sy = s->sys; sampler_unlock(s);
    double total = (double)sy.phys_total, availp = (double)sy.phys_avail;
    j_begin_obj(&j);
    j_key(&j, "system"); j_begin_obj(&j);
    j_key(&j, "total"); j_begin_obj(&j);
    j_key(&j, "text"); fmt_bytes(total / 1048576.0, tmp, sizeof tmp); j_strn(&j, tmp, strlen(tmp));
    j_key(&j, "mb"); j_dbl(&j, total / 1048576.0);
    j_end(&j);
    j_key(&j, "avail"); j_begin_obj(&j);
    j_key(&j, "text"); fmt_bytes(availp / 1048576.0, tmp, sizeof tmp); j_strn(&j, tmp, strlen(tmp));
    j_key(&j, "mb"); j_dbl(&j, availp / 1048576.0);
    j_end(&j);
    j_key(&j, "avail_pct"); j_dbl(&j, total ? availp / total * 100 : 0);
    j_key(&j, "commit_mb"); j_dbl(&j, (double)sy.commit_used / 1048576.0);
    j_key(&j, "commit_limit_mb"); j_dbl(&j, (double)sy.commit_limit / 1048576.0);
    j_key(&j, "cache_mb"); j_dbl(&j, (double)sy.cache_bytes / 1048576.0);
    j_key(&j, "handles"); j_uint(&j, sy.handle_count);
    j_key(&j, "threads"); j_uint(&j, sy.thread_count);
    j_end(&j);
    /* top 进程 */
    j_key(&j, "top"); j_begin_arr(&j);
    hotspot_item hh[10];
    sampler_lock(s);
    int hn = hotspot_rank(s, "private", 10, hh, 10);
    for (int i = 0; i < hn; i++) {
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, hh[i].pid);
        j_key(&j, "name"); j_str(&j, hh[i].name);
        j_key(&j, "runtime"); j_str(&j, runtime_label((rt_kind)hh[i].rt));
        j_key(&j, "private_mb"); j_dbl(&j, hh[i].private_mb);
        j_key(&j, "ws_mb"); j_dbl(&j, hh[i].ws_mb);
        j_key(&j, "peak_mb"); j_dbl(&j, hh[i].peak_mb);
        j_key(&j, "cpu_pct"); j_dbl(&j, hh[i].cpu_pct);
        j_key(&j, "share"); j_dbl(&j, hh[i].share);
        j_end(&j);
    }
    j_end(&j);
    /* 采样器状态 */
    j_key(&j, "sampler"); j_begin_obj(&j);
    j_key(&j, "interval_ms"); j_int(&j, s->cfg.interval_ms);
    j_key(&j, "window_size"); j_int(&j, s->cfg.window_size);
    j_key(&j, "sample_count"); j_uint(&j, s->sample_count);
    j_key(&j, "uptime_s"); j_int(&j, (int64_t)((now_ms() - s->started_ms) / 1000));
    j_end(&j);
    sampler_unlock(s);
    j_end(&j);
    return jresp(&j, 200);
}

static http_resp api_processes(const app_ctx *ctx, const http_req *req) {
    sampler *s = ctx->sampler;
    char sort[32], srch[96], stop[16];
    q_param(req, "sort", sort, sizeof sort);
    q_param(req, "search", srch, sizeof srch);
    q_param(req, "top", stop, sizeof stop);
    int top = atoi(stop); if (top <= 0 || top > 2000) top = 2000;

    sampler_lock(s);
    int n = s->nprocs;
    json j; json_init(&j, 1 << 12);
    j_begin_obj(&j);
    j_key(&j, "count"); j_int(&j, n);
    j_key(&j, "processes"); j_begin_arr(&j);
    int shown = 0;
    for (int i = 0; i < n && shown < top; i++) {
        proc_snap *p = &s->procs[i];
        if (srch[0] && !strstr(p->name, srch)) continue;
        double cpu = cpu_pct_for(s, p->pid);
        proc_to_json(&j, p, cpu);
        shown++;
    }
    j_end(&j);
    j_end(&j);
    sampler_unlock(s);
    return jresp(&j, 200);
}

static http_resp api_process_single(const app_ctx *ctx, uint32_t pid) {
    sampler *s = ctx->sampler;
    proc_snap cur;
    mt_err e = counters_process(pid, &cur);
    if (e == MT_ERR_NOT_FOUND) return json_err(404, "进程不存在");
    json j; json_init(&j, 1 << 10);
    j_begin_obj(&j);
    j_key(&j, "process");
    if (e == MT_ERR_PERMISSION) {
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, pid);
        j_key(&j, "permission_limited"); j_bool(&j, 1);
        j_end(&j);
    } else proc_to_json(&j, &cur, cpu_pct_for(s, pid));
    j_end(&j);
    return jresp(&j, 200);
}

static http_resp api_trend(const app_ctx *ctx, uint32_t pid) {
    sampler *s = ctx->sampler;
    sampler_lock(s);
    sseries *se = pid == 0 ? s->sys_series : sampler_find_series(s, pid);
    json j; json_init(&j, 1 << 12);
    j_begin_obj(&j);
    if (!se) { j_key(&j, "error"); j_str(&j, "无该进程数据"); }
    else {
        j_key(&j, "pid"); j_uint(&j, se->pid);
        j_key(&j, "points"); j_uint(&j, se->len);
        int cap = (int)se->len; if (cap > 2000) cap = 2000;
        j_key(&j, "t"); j_begin_arr(&j);
        for (int i = 0; i < cap; i++) {
            uint32_t idx = (se->head + se->cap - (uint32_t)cap + (uint32_t)i) % se->cap;
            j_dbl(&j, (double)(se->ts[idx] / 1000.0));
        }
        j_end(&j);
        j_key(&j, "priv_mb"); j_begin_arr(&j);
        for (int i = 0; i < cap; i++) { uint32_t idx=(se->head+se->cap-(uint32_t)cap+(uint32_t)i)%se->cap; j_dbl(&j, se->priv[idx]/1048576.0); }
        j_end(&j);
        j_key(&j, "ws_mb"); j_begin_arr(&j);
        for (int i = 0; i < cap; i++) { uint32_t idx=(se->head+se->cap-(uint32_t)cap+(uint32_t)i)%se->cap; j_dbl(&j, se->ws[idx]/1048576.0); }
        j_end(&j);
        if (pid != 0) {
            j_key(&j, "cpu"); j_begin_arr(&j);
            double prev_t=se->ts[se->head==0?se->cap-1:se->head-1], prev_c=se->cpu[se->head==0?se->cap-1:se->head-1];
            for (int i = 0; i < cap; i++) {
                uint32_t idx=(se->head+se->cap-(uint32_t)cap+(uint32_t)i)%se->cap;
                double dt=(double)(se->ts[idx]-prev_t)/1000.0;
                double p = dt>0 ? (se->cpu[idx]-prev_c)/dt*100.0 : 0;
                j_dbl(&j, p<0?0:p);
                prev_t=se->ts[idx]; prev_c=se->cpu[idx];
            }
            j_end(&j);
        }
    }
    j_end(&j);
    sampler_unlock(s);
    return jresp(&j, 200);
}

static http_resp api_leaks(const app_ctx *ctx) {
    sampler_lock(ctx->sampler);
    leak_item li[50];
    int n = leak_detect(ctx->sampler, li, 50);
    json j; json_init(&j, 1 << 10);
    j_begin_obj(&j);
    j_key(&j, "count"); j_int(&j, n);
    j_key(&j, "leaks"); j_begin_arr(&j);
    for (int i = 0; i < n; i++) {
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, li[i].pid);
        j_key(&j, "name"); j_str(&j, li[i].name);
        j_key(&j, "kind"); j_str(&j, li[i].kind == 0 ? "私有字节" : "句柄");
        j_key(&j, "rate_kb_min"); j_dbl(&j, li[i].slope_kb_per_min);
        j_key(&j, "r2"); j_dbl(&j, li[i].r2);
        j_key(&j, "span_sec"); j_dbl(&j, li[i].time_span_sec);
        j_key(&j, "cpu_pct"); j_dbl(&j, li[i].cpu_pct);
        j_key(&j, "severity"); j_int(&j, li[i].severity);
        j_end(&j);
    }
    j_end(&j);
    j_end(&j);
    sampler_unlock(ctx->sampler);
    return jresp(&j, 200);
}

static http_resp api_hotspots(const app_ctx *ctx, const http_req *req) {
    char metric[16], top[16];
    q_param(req, "metric", metric, sizeof metric);
    q_param(req, "top", top, sizeof top);
    if (!metric[0]) snprintf(metric, sizeof metric, "private");
    int t = atoi(top); if (t <= 0 || t > 100) t = 10;
    sampler_lock(ctx->sampler);
    hotspot_item hh[100];
    int n = hotspot_rank(ctx->sampler, metric, t, hh, 100);
    json j; json_init(&j, 1 << 10);
    j_begin_obj(&j);
    j_key(&j, "metric"); j_str(&j, metric);
    j_key(&j, "count"); j_int(&j, n);
    j_key(&j, "hotspots"); j_begin_arr(&j);
    for (int i = 0; i < n; i++) {
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, hh[i].pid);
        j_key(&j, "name"); j_str(&j, hh[i].name);
        j_key(&j, "runtime"); j_str(&j, runtime_label((rt_kind)hh[i].rt));
        j_key(&j, "private_mb"); j_dbl(&j, hh[i].private_mb);
        j_key(&j, "ws_mb"); j_dbl(&j, hh[i].ws_mb);
        j_key(&j, "peak_mb"); j_dbl(&j, hh[i].peak_mb);
        j_key(&j, "cpu_pct"); j_dbl(&j, hh[i].cpu_pct);
        j_key(&j, "threads"); j_uint(&j, hh[i].threads);
        j_key(&j, "share"); j_dbl(&j, hh[i].share);
        j_end(&j);
    }
    j_end(&j);
    j_end(&j);
    sampler_unlock(ctx->sampler);
    return jresp(&j, 200);
}

static http_resp api_fragmentation(const app_ctx *ctx, uint32_t pid) {
    (void)ctx;
    frag_result fr;
    int e = fragment_analyze(pid, &fr);
    json j; json_init(&j, 1 << 8);
    j_begin_obj(&j);
    j_key(&j, "pid"); j_uint(&j, pid);
    j_key(&j, "ok"); j_bool(&j, fr.ok);
    if (!fr.ok) { j_key(&j, "error"); j_str(&j, fr.error); }
    else {
        j_key(&j, "region_count"); j_int(&j, fr.region_count);
        j_key(&j, "committed_mb"); j_dbl(&j, (double)fr.total_committed / 1048576.0);
        j_key(&j, "largest_region_mb"); j_dbl(&j, (double)fr.largest_region / 1048576.0);
        j_key(&j, "reserved_mb"); j_dbl(&j, (double)fr.total_reserved / 1048576.0);
        j_key(&j, "frag_idx"); j_dbl(&j, fr.frag_idx);
    }
    j_end(&j);
    (void)e;
    return jresp(&j, 200);
}

static http_resp api_runtime(const app_ctx *ctx) {
    sampler *s = ctx->sampler;
    sampler_lock(s);
    json j; json_init(&j, 1 << 8);
    j_begin_obj(&j);
    j_key(&j, "runtimes"); j_begin_arr(&j);
    /* 汇总各运行时进程数 */
    static const rt_kind kinds[] = { RT_UNKNOWN, RT_PYTHON, RT_JVM, RT_NODE, RT_POWERSHELL,
                                     RT_CMD, RT_DOTNET, RT_GO, RT_RUST, RT_CPP_NATIVE };
    int nc = (int)(sizeof kinds / sizeof kinds[0]);
    int *cnt = (int*)calloc((size_t)nc, sizeof(int));
    if (!cnt) { j_end(&j); j_end(&j); return jresp(&j, 200); }
    for (int i = 0; i < s->nprocs; i++) {
        rt_kind k = (rt_kind)s->procs[i].rt;
        for (int q = 0; q < nc; q++) if (kinds[q] == k) { cnt[q]++; break; }
    }
    for (int q = 0; q < nc; q++) {
        j_begin_obj(&j);
        j_key(&j, "runtime"); j_str(&j, runtime_label(kinds[q]));
        j_key(&j, "count"); j_int(&j, cnt[q]);
        j_end(&j);
    }
    free(cnt);
    j_end(&j);
    j_end(&j);
    sampler_unlock(s);
    return jresp(&j, 200);
}

static http_resp api_suggestions(const app_ctx *ctx) {
    sampler_lock(ctx->sampler);
    suggest_item su[30];
    int n = optimize_analyze(ctx->sampler, su, 30);
    json j; json_init(&j, 1 << 10);
    j_begin_obj(&j);
    j_key(&j, "count"); j_int(&j, n);
    j_key(&j, "suggestions"); j_begin_arr(&j);
    for (int i = 0; i < n; i++) {
        j_begin_obj(&j);
        j_key(&j, "severity"); j_int(&j, su[i].severity);
        j_key(&j, "kind"); j_str(&j,
            su[i].kind == SUG_RECLAIM ? "回收" : su[i].kind == SUG_LARGE_OBJ ? "大对象" :
            su[i].kind == SUG_CACHE ? "缓存" : su[i].kind == SUG_LEAK ? "泄漏" : "系统");
        j_key(&j, "pid"); j_uint(&j, su[i].pid);
        j_key(&j, "name"); j_str(&j, su[i].name);
        j_key(&j, "title"); j_str(&j, su[i].title);
        j_key(&j, "reason"); j_str(&j, su[i].reason);
        j_key(&j, "actionable"); j_bool(&j, su[i].actionable);
        j_key(&j, "action"); j_str(&j, su[i].action);
        j_end(&j);
    }
    j_end(&j);
    j_end(&j);
    sampler_unlock(ctx->sampler);
    return jresp(&j, 200);
}

static http_resp api_apply(const app_ctx *ctx, const http_req *req) {
    int64_t pid = 0; int found = 0;
    json_get_int(req->body, "pid", &pid, &found);
    if (!found || pid <= 0) return json_err(400, "缺少 pid");
    char action[32] = "reclaim";
    json_get_str(req->body, "action", action, sizeof action, &found);
    char err[256];
    mt_err e = optimize_apply(ctx->sampler, &ctx->prot, (uint32_t)pid, action, err, sizeof err);
    json j; json_init(&j, 1 << 8);
    j_begin_obj(&j);
    j_key(&j, "ok"); j_bool(&j, e == MT_OK);
    j_key(&j, "pid"); j_uint(&j, (uint32_t)pid);
    j_key(&j, "action"); j_str(&j, action);
    if (e != MT_OK) { j_key(&j, "error"); j_str(&j, err); }
    else { j_key(&j, "message"); j_str(&j, "工作集回收已执行"); }
    j_end(&j);
    return jresp(&j, e == MT_OK ? 200 : 500);
}

static http_resp api_optimize(const app_ctx *ctx, const http_req *req) {
    oneclick_result oc;
    unsigned opts = 0;
    int64_t ov = 0; int found = 0;
    json_get_int(req->body, "options", &ov, &found);
    if (found) opts = (unsigned)ov;
    mt_err e = optimize_oneclick(ctx->sampler, &ctx->prot, &oc, opts);
    json j; json_init(&j, 1 << 12);
    j_begin_obj(&j);
    j_key(&j, "ok"); j_bool(&j, e == MT_OK);
    j_key(&j, "reclaimed"); j_int(&j, oc.reclaimed);
    j_key(&j, "killed"); j_int(&j, oc.killed);
    j_key(&j, "skipped_protected"); j_int(&j, oc.skipped_protected);
    j_key(&j, "skipped_perm"); j_int(&j, oc.skipped_perm);
    j_key(&j, "reclaimed_mb"); j_dbl(&j, oc.reclaimed_mb);
    j_key(&j, "cache_purged"); j_bool(&j, oc.cache_purged);
    if (oc.cache_error[0]) { j_key(&j, "cache_error"); j_str(&j, oc.cache_error); }
    j_key(&j, "killed_list"); j_begin_arr(&j);
    for (int i = 0; i < oc.killed && i < 16; i++) {
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, oc.killed_list[i].pid);
        j_key(&j, "name"); j_str(&j, oc.killed_list[i].name);
        j_end(&j);
    }
    j_end(&j);
    j_key(&j, "detail"); j_str(&j, oc.detail);
    j_end(&j);
    return jresp(&j, e == MT_OK ? 200 : 500);
}

static http_resp api_report(const app_ctx *ctx, const http_req *req) {
    char fmt[8], pid[16];
    q_param(req, "format", fmt, sizeof fmt);
    q_param(req, "pid", pid, sizeof pid);
    uint32_t p = (uint32_t)atoi(pid);

    /* Content-Disposition 头必须驻留到响应发送完毕，故堆分配并由服务器 free */
    http_resp r; memset(&r, 0, sizeof r);

    if (!strcmp(fmt, "csv")) {
        char *csv = report_build_csv(ctx->sampler, p);
        if (!csv) return json_err(500, "报告生成失败");
        r = text_resp(csv, 200, "text/csv; charset=utf-8", 1);
        r.extra_headers = _strdup("Content-Disposition: attachment; filename=\"report.csv\"\r\n");
        r.free_extra = r.extra_headers != NULL;
        return r;
    }
    if (!strcmp(fmt, "json")) {
        char *js = report_build_json(ctx->sampler);
        if (!js) return json_err(500, "报告生成失败");
        r = text_resp(js, 200, "application/json; charset=utf-8", 1);
        r.extra_headers = _strdup("Content-Disposition: attachment; filename=\"report.json\"\r\n");
        r.free_extra = r.extra_headers != NULL;
        return r;
    }
    if (!strcmp(fmt, "xls")) {
        char *xls = report_build_xls(ctx->sampler);
        if (!xls) return json_err(500, "报告生成失败");
        r = text_resp(xls, 200, "application/vnd.ms-excel; charset=utf-8", 1);
        r.extra_headers = _strdup("Content-Disposition: attachment; filename=\"report.xls\"\r\n");
        r.free_extra = r.extra_headers != NULL;
        return r;
    }
    if (!strcmp(fmt, "pdf")) {
        size_t len = 0;
        char *pdf = pdf_build_report(ctx->sampler, ctx->cfg.pdf_font, &len);
        if (!pdf) return json_err(500, "PDF 生成失败");
        r.status = 200; r.content_type = "application/pdf";
        r.body = pdf; r.body_len = len; r.free_body = 1;
        r.extra_headers = _strdup("Content-Disposition: attachment; filename=\"report.pdf\"\r\n");
        r.free_extra = r.extra_headers != NULL;
        return r;
    }
    if (fmt[0] && strcmp(fmt, "html") != 0) return json_err(400, "不支持的格式");
    char *html = report_build_html(ctx->sampler);
    return text_resp(html, 200, "text/html; charset=utf-8", 1);
}

static http_resp api_sampler(const app_ctx *ctx) {
    sampler *s = ctx->sampler;
    json j; json_init(&j, 1 << 8);
    j_begin_obj(&j);
    j_key(&j, "interval_ms"); j_int(&j, s->cfg.interval_ms);
    j_key(&j, "window_size"); j_int(&j, s->cfg.window_size);
    j_key(&j, "sample_count"); j_uint(&j, s->sample_count);
    j_key(&j, "uptime_s"); j_int(&j, (int64_t)((now_ms() - s->started_ms) / 1000));
    j_key(&j, "nseries"); j_int(&j, s->nseries);
    j_end(&j);
    return jresp(&j, 200);
}

/* POST /api/v1/protect：op=list|add|remove，管理受保护进程名单 */
static http_resp api_protect(const app_ctx *ctx, const http_req *req) {
    char op[16] = "list";
    int found = 0;
    json_get_str(req->body, "op", op, sizeof op, &found);
    if (!strcmp(op, "list")) {
        json j; json_init(&j, 1 << 10);
        j_begin_obj(&j);
        j_key(&j, "count"); j_int(&j, ctx->prot.count);
        j_key(&j, "names"); j_begin_arr(&j);
        for (int i = 0; i < ctx->prot.count; i++) j_str(&j, ctx->prot.names[i]);
        j_end(&j);
        j_end(&j);
        return jresp(&j, 200);
    }
    if (!strcmp(op, "add") || !strcmp(op, "remove")) {
        char name[96] = "";
        json_get_str(req->body, "name", name, sizeof name, &found);
        if (!found || !name[0]) return json_err(400, "缺少 name");
        /* 内置系统名单不可被用户重复加入（但允许从用户名单移除同名项） */
        if (op[0] == 'a' && prot_is_builtin(name)) {
            int in_pl = 0;
            for (int i = 0; i < ctx->prot.count; i++)
                if (!strcmp(ctx->prot.names[i], name)) { in_pl = 1; break; }
            if (!in_pl) return json_err(400, "该进程名与内置系统名单重复，无需手动加入");
        }
        char err[256]; (void)err;
        mt_err e;
        if (op[0] == 'a') e = protect_add((protect_list*)&ctx->prot, ctx->cfg.data_dir, name);
        else e = protect_remove((protect_list*)&ctx->prot, ctx->cfg.data_dir, name);
        json j; json_init(&j, 1 << 8);
        j_begin_obj(&j);
        j_key(&j, "ok"); j_bool(&j, e == MT_OK);
        j_key(&j, "op"); j_str(&j, op);
        if (e != MT_OK) {
            j_key(&j, "error");
            if (e == MT_ERR_NOT_FOUND) j_str(&j, "名单中不存在该进程名");
            else if (e == MT_ERR_BUSY) j_str(&j, "名单已满");
            else j_str(&j, "操作失败");
        } else {
            j_key(&j, "count"); j_int(&j, ctx->prot.count);
            j_key(&j, "message"); j_str(&j, op[0] == 'a' ? "已加入保护名单" : "已从保护名单移除");
        }
        j_end(&j);
        (void)err;
        return jresp(&j, e == MT_OK ? 200 : 400);
    }
    return json_err(400, "未知 op");
}

/* ---------- 顶层分发 ---------- */
http_resp routes_handler(const http_req *req, void *ctx) {
    app_ctx *a = (app_ctx*)ctx;

    char err[512];
    int access = check_access(a, req, NULL, 0, err, sizeof err);
    if (access) return json_err(access, err);

    /* CORS 响应头（若 Origin 被放行） */
    char origin[512]; int has_origin = header_has(req, "Origin", origin, sizeof origin) == 0;
    char cors_hdr[600] = "";
    if (has_origin) snprintf(cors_hdr, sizeof cors_hdr, "Access-Control-Allow-Origin: %s\r\n", origin);

    /* 静态资源（非 /api） */
    if (strncmp(req->target, "/api/v1/", 8) != 0) {
        http_resp r = serve_static(req->target, NULL);
        if (r.status == 200 && cors_hdr[0]) r.extra_headers = cors_hdr;
        return r;
    }

    /* API 分发 */
    const char *t = req->target + 8;      /* 去掉 "/api/v1/" */
    http_resp r; memset(&r, 0, sizeof r);

    if (!strcmp(t, "overview")) r = api_overview(a, req);
    else if (!strcmp(t, "processes")) r = api_processes(a, req);
    else if (!strcmp(t, "leaks")) r = api_leaks(a);
    else if (!strncmp(t, "hotspots", 8)) r = api_hotspots(a, req);
    else if (!strcmp(t, "suggestions")) r = api_suggestions(a);
    else if (!strcmp(t, "suggestions/apply")) r = api_apply(a, req);
    else if (!strcmp(t, "optimize")) r = api_optimize(a, req);
    else if (!strcmp(t, "protect")) r = api_protect(a, req);
    else if (!strncmp(t, "process/", 8)) {
        const char *rest = t + 8;
        uint32_t pid = (uint32_t)atol(rest);
        if (strstr(rest, "/trend")) r = api_trend(a, pid);
        else r = api_process_single(a, pid);
    }
    else if (!strncmp(t, "fragmentation/", 14)) r = api_fragmentation(a, (uint32_t)atol(t + 14));
    else if (!strcmp(t, "runtime")) r = api_runtime(a);
    else if (!strncmp(t, "report", 6)) r = api_report(a, req);
    else if (!strcmp(t, "sampler")) r = api_sampler(a);
    else return json_err(404, "未找到接口");

    if (cors_hdr[0] && r.body==NULL) r.extra_headers = cors_hdr;
    return r;
}