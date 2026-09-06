#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include <stdbool.h>

typedef struct http_req {
    char method[8];
    char target[512];    /* 路径 */
    char query[512];     /* 查询串（未解码） */
    const char *header;  /* 原始请求头 */
    const char *body;    /* POST body */
    size_t body_len;
    char peer_ip[64];    /* 客户端地址 */
} http_req;

typedef struct http_resp {
    int status;              /* 200/400/401/403/404/500 ... */
    const char *content_type;
    const char *body;
    size_t body_len;
    const char *extra_headers; /* 可选的额外响应头（形如 "X-Foo: bar\r\n"），可为 NULL */
    int free_body;            /* 1=服务器发送后 free(body) */
    int free_extra;           /* 1=服务器发送后 free(extra_headers)（调用方 malloc 时置位） */
} http_resp;

typedef http_resp (*http_handler)(const http_req *req, void *ctx);

/* 阻塞启动 HTTP 服务，直到 *running 置 false。
 * 每连接一处理线程；返回 0 成功。 */
int http_run(const char *host, int port, http_handler handler, void *ctx, volatile bool *running);

#endif