#ifndef MEMTOOL_H
#define MEMTOOL_H

/* 高效内存优化工具 - 公共定义
 * 纯 C(C11) + MinGW-w64，监控 Windows 本机进程 */

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* 错误码 */
typedef enum {
    MT_OK = 0,
    MT_ERR_PARAM = -1,
    MT_ERR_PERMISSION = -2,   /* 权限不足（受保护进程/需管理员） */
    MT_ERR_NOT_FOUND = -3,    /* 找不到进程/PID */
    MT_ERR_SYSTEM = -4,       /* 系统调用失败 */
    MT_ERR_BUSY = -5,
    MT_ERR_INVALID = -6,      /* 非法参数（如路径穿越） */
} mt_err;

#define MT_KB(x) ((double)(x) / 1024.0)
#define MT_MB(x) ((double)(x) / (1024.0 * 1024.0))

/* 运行时类型（用于“多种语言/运行环境”识别） */
typedef enum {
    RT_UNKNOWN = 0,
    RT_PYTHON,
    RT_JVM,       /* java/javaw/各种 JVM 发行版 */
    RT_NODE,
    RT_POWERSHELL,
    RT_CMD,
    RT_DOTNET,    /* dotnet/csc */
    RT_GO,
    RT_RUST,
    RT_CPP_NATIVE, /* 原生二进制（非明显运行时） */
} rt_kind;

/* 单进程快照（由 snapshot 填充） */
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    char     name[96];      /* 可执行文件名（不含路径） */
    uint64_t working_set;   /* 当前工作集 bytes */
    uint64_t private_bytes; /* 私有字节 bytes */
    uint64_t page_file_usage;
    uint64_t page_faults;   /* 累计缺页 */
    uint32_t handle_count;
    uint32_t thread_count;
    uint64_t kernel_ms;     /* 累计内核 CPU ms */
    uint64_t user_ms;       /* 累计用户 CPU ms */
    uint64_t virtual_size;
    uint64_t peak_working_set;
    int      rt;            /* rt_kind */
} proc_snap;

/* 系统级内存统计 */
typedef struct {
    uint64_t phys_total;
    uint64_t phys_avail;
    uint64_t commit_limit;
    uint64_t commit_used;
    uint64_t pagefile_total;
    uint64_t pagefile_free;
    uint64_t cache_bytes;   /* 缓存/standby 字节估算 */
    uint32_t handle_count;
    uint32_t thread_count;
} sys_mem;

/* 配置 */
typedef struct {
    char host[64];
    int  port;
    char api_token[128];     /* 空 = 本地免密；allow_remote=1 时必填 */
    int  allow_remote;
    char cors_origin[512];   /* 逗号分隔白名单，空的场合中仅本机(127.0.0.1)请求合法 */
    int  interval_ms;        /* 采样间隔 ms */
    int  window_size;        /* 单进程时间序列环形缓冲点数 */
    int  storage_enabled;
    char data_dir[256];
    int  retention_hours;
    int  auto_watch_top;     /* 自动纳入趋势监控的 top N 进程数 */
    char pdf_font[320];      /* PDF 导出中文字体（将提取字形子集），空=用系统默认 */
} memtool_config;

extern void config_defaults(memtool_config *cfg);
extern mt_err config_load(memtool_config *cfg, const char *path);
extern void config_print(const memtool_config *cfg, FILE *out);

/* 简单动态字符串（供 JSON/报告拼接） */
typedef struct strbuf {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} strbuf;
void sb_init(strbuf *s, size_t cap);
void sb_free(strbuf *s);
void sb_putc(strbuf *s, int c);
void sb_puts(strbuf *s, const char *v);
void sb_putn(strbuf *s, const char *v, size_t n);
const char *sb_cstr(strbuf *s);   /* 确保 NUL 结尾并返回指针 */
void sb_printf(strbuf *s, const char *fmt, ...);

/* 时间辅助 */
uint64_t now_ms(void);            /* GetTickCount64 (uptime) */
uint64_t wall_ms(void);           /* GetSystemTimeAsFileTime 毫秒 */

#endif /* MEMTOOL_H */