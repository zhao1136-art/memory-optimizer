/* 入口：CLI 解析、serve 主循环、--demo 自测子进程、--snapshot/--report */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "memtool.h"
#include "sampler.h"
#include "routes.h"
#include "storage.h"
#include "snapshot.h"
#include "json.h"
#include "counters.h"
#include "runtime.h"
#include "leak.h"
#include "hotspot.h"
#include "report.h"
#include "pdf.h"
#include "protect.h"

static volatile bool g_running = true;

/* —— 自测/演示 子进程模式 —— */

/* 泄漏进程：每 5 秒分配 4MB 且不释放（私有字节持续增长） */
static int run_leak_child(void) {
    fprintf(stderr, "[leakchild] 开始持续分配（不释放）...\n");
    size_t chunk = 4u * 1024 * 1024;
    for (;;) {
        void *p = malloc(chunk);
        if (p) memset(p, 1, 4096);   /* 触碰首 4KB，确保提交 */
        /* 不 free -> 泄漏，私有字节增长 */
        Sleep(5000);
    }
    return 0;
}

/* 高驻留进程：分配并驻留 MB 字节 */
static int run_resident_child(int mb) {
    if (mb <= 0) mb = 256;
    size_t total = (size_t)mb * 1024 * 1024;
    char *b = (char*)malloc(total);
    if (b) {
        size_t step = 4096;
        for (size_t i = 0; i < total; i += step) b[i] = 1;   /* 触所有页 */
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)total, (SIZE_T)total);
        fprintf(stderr, "[residentchild] 已驻留 %d MB\n", mb);
    } else fprintf(stderr, "[residentchild] 分配失败\n");
    for (;;) Sleep(60000);
    return 0;
}

static void spawn_child(const char *args) {
    char self[MAX_PATH];
    if (!GetModuleFileNameA(NULL, self, MAX_PATH)) return;
    char cmd[900];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, args);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

/* —— 单次快照 —— */
static int do_snapshot(void) {
    proc_snap *arr = NULL; sys_mem sy;
    int n = snapshot_procs(&arr, &sy);
    json j; json_init(&j, 1 << 12);
    j_begin_obj(&j);
    j_key(&j, "system"); j_begin_obj(&j);
    j_key(&j, "total_mb"); j_dbl(&j, (double)sy.phys_total / 1048576.0);
    j_key(&j, "avail_mb"); j_dbl(&j, (double)sy.phys_avail / 1048576.0);
    j_key(&j, "commit_mb"); j_dbl(&j, (double)sy.commit_used / 1048576.0);
    j_key(&j, "cache_mb"); j_dbl(&j, (double)sy.cache_bytes / 1048576.0);
    j_end(&j);
    j_key(&j, "processes"); j_begin_arr(&j);
    for (int i = 0; i < n; i++) {
        proc_snap *p = &arr[i];
        j_begin_obj(&j);
        j_key(&j, "pid"); j_uint(&j, p->pid);
        j_key(&j, "name"); j_str(&j, p->name);
        j_key(&j, "runtime"); j_str(&j, runtime_label((rt_kind)p->rt));
        j_key(&j, "private_mb"); j_dbl(&j, (double)p->private_bytes / 1048576.0);
        j_key(&j, "ws_mb"); j_dbl(&j, (double)p->working_set / 1048576.0);
        j_end(&j);
    }
    j_end(&j);
    j_end(&j);
    printf("%s\n", json_cstr(&j));
    json_dtor(&j);
    free(arr);
    return 0;
}

/* —— 存储 watchdog 线程 —— */
static DWORD WINAPI storage_watchdog(LPVOID arg) {
    struct { storage *st; sampler *sm; memtool_config *cfg; } *p = (void*)arg;
    while (g_running) {
        Sleep(30000);
        storage_tick(p->st, p->sm);
        storage_cleanup(p->st, p->cfg->retention_hours);
    }
    return 0;
}

/* —— 帮助 —— */
static void usage(void) {
    fprintf(stderr,
        "MemOptimizer - 高效内存优化工具 (纯 C / Windows 进程监控)\n\n"
        "用法: memtool [选项]\n"
        "  --config <path>      指定配置文件(memtool.conf)\n"
        "  --host <ip>          监听地址(默认 127.0.0.1)\n"
        "  --port <port>        监听端口(默认 8080)\n"
        "  --interval <ms>      采样间隔(默认 2000)\n"
        "  --serve              启动 Web 服务(默认)\n"
        "  --snapshot           输出一次系统+进程全景 JSON 后退出\n"
        "  --report <fmt>       fmt=html|json|csv|xls|pdf，生成报告（pdf 输出 report.pdf，其余到 stdout）\n"
        "  --demo               生成泄漏+高驻留测试进程并启动服务\n"
        "  --leakchild          以泄漏子进程模式运行(内部使用)\n"
        "  --residentchild <MB> 以高驻留子进程模式运行(内部使用)\n"
        "  -h, --help           显示本帮助\n");
}

int main(int argc, char **argv) {
    memtool_config cfg; config_defaults(&cfg);
    const char *cfgpath = NULL;
    int do_snapshot_flag = 0, demo = 0;
    char report_fmt[16] = ""; uint32_t report_pid = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgpath = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) snprintf(cfg.host, sizeof cfg.host, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) cfg.interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot")) do_snapshot_flag = 1;
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) snprintf(report_fmt, sizeof report_fmt, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) report_pid = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--leakchild")) return run_leak_child();
        else if (!strcmp(argv[i], "--residentchild")) {
            int mb = 256;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) mb = atoi(argv[++i]);
            return run_resident_child(mb);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
    }

    config_load(&cfg, cfgpath);
    if (cfg.interval_ms < 200) cfg.interval_ms = 200;

    if (do_snapshot_flag) return do_snapshot();

    /* 报告模式：启动采样器收集后渲染 */
    if (report_fmt[0]) {
        sampler sm; sampler_start(&sm, &cfg);
        Sleep(2500);
        if (!strcmp(report_fmt, "json")) {
            char *b = report_build_json(&sm);
            if (b) { fputs(b, stdout); free(b); }
        } else if (!strcmp(report_fmt, "html")) {
            char *h = report_build_html(&sm);
            if (h) { fputs(h, stdout); free(h); }
        } else if (!strcmp(report_fmt, "csv")) {
            char *c = report_build_csv(&sm, report_pid);
            if (c) { fputs(c, stdout); free(c); }
        } else if (!strcmp(report_fmt, "xls")) {
            char *x = report_build_xls(&sm);
            if (x) { fputs(x, stdout); free(x); }
        } else if (!strcmp(report_fmt, "pdf")) {
            size_t len = 0;
            char *p = pdf_build_report(&sm, cfg.pdf_font, &len);
            if (p) {
                FILE *f = fopen("report.pdf", "wb");
                if (f) { fwrite(p, 1, len, f); fclose(f); printf("已写出 report.pdf (%llu 字节)\n", (unsigned long long)len); }
                else fprintf(stderr, "无法写 report.pdf\n");
                free(p);
            } else fprintf(stderr, "PDF 生成失败\n");
        } else {
            fprintf(stderr, "不支持的报告格式: %s\n", report_fmt);
        }
        sampler_stop(&sm);
        return 0;
    }

    /* 服务模式 */
    storage store; storage_init(&store, &cfg);
    sampler sm; 
    if (sampler_start(&sm, &cfg) != 0) {
        fprintf(stderr, "无法启动采样器\n");
        return 1;
    }

    if (demo) {
        spawn_child("--leakchild");
        spawn_child("--residentchild 256");
        fprintf(stderr, "已生成演示进程（泄漏 + 高驻留）。\n");
    }

    printf("MemOptimizer 已启动：%s:%d （打开 http://%s:%d 查看 Web 后台）\n",
           cfg.host, cfg.port, cfg.host, cfg.port);
    config_print(&cfg, stderr);

    struct { storage *st; sampler *sm; memtool_config *cfg; } wdarg = { &store, &sm, &cfg };
    HANDLE wd = CreateThread(NULL, 0, storage_watchdog, &wdarg, 0, NULL);

    app_ctx app; memset(&app, 0, sizeof app);
    app.sampler = &sm; app.cfg = cfg; app.running = &g_running; app.store = &store;
    protect_load(&app.prot, cfg.data_dir);

    g_running = true;
    http_run(cfg.host, cfg.port, routes_handler, &app, &g_running);
    g_running = false;

    if (wd) { WaitForSingleObject(wd, 1000); CloseHandle(wd); }
    sampler_stop(&sm);
    storage_close(&store);
    return 0;
}