#ifndef ROUTES_H
#define ROUTES_H
#include "http_server.h"
#include "sampler.h"
#include "storage.h"
#include "protect.h"

typedef struct app_ctx {
    sampler *sampler;
    memtool_config cfg;
    volatile bool *running;
    storage *store;
    protect_list prot;      /* 受保护进程名单（用户可增删） */
} app_ctx;

/* HTTP 顶层处理器：做鉴权/路径校验后分发到 REST 或静态资源 */
http_resp routes_handler(const http_req *req, void *ctx);

#endif