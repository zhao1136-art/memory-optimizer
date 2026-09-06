/* 极简多线程 HTTP/1.1 服务器（WinSock）。每连接一线程，Connection: close。 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "memtool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "http_server.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#define HDR_CAP 65536

static void http_send_all(SOCKET s, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(s, buf + off, len - off, 0);
        if (n <= 0) break;
        off += n;
    }
}

typedef struct conn_ctx {
    SOCKET s;
    http_handler handler;
    void *ctx;
} conn_ctx;

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

static void handle_conn(SOCKET s, http_handler handler, void *ctx) {
    char *hbuf = (char*)malloc(HDR_CAP);
    if (!hbuf) { closesocket(s); return; }
    int total = 0;
    /* 读取请求头（直到 \r\n\r\n） */
    int header_end = -1;
    while (total < HDR_CAP - 1) {
        int r = recv(s, hbuf + total, (int)(HDR_CAP - 1 - total), 0);
        if (r <= 0) break;
        total += r;
        hbuf[total] = '\0';
        char *he = strstr(hbuf, "\r\n\r\n");
        if (he) { header_end = (int)(he - hbuf); break; }
    }
    if (header_end < 0) { free(hbuf); closesocket(s); return; }

    http_req req; memset(&req, 0, sizeof req);
    req.header = hbuf;
    {
        struct sockaddr_in pa; int pl = sizeof pa;
        if (getpeername(s, (struct sockaddr*)&pa, &pl) == 0)
            inet_ntop(AF_INET, &pa.sin_addr, req.peer_ip, sizeof req.peer_ip);
    }
    /* 首行 */
    char *line_end = strstr(hbuf, "\r\n");
    if (!line_end) { free(hbuf); closesocket(s); return; }
    *line_end = ' ';   /* 用空格替代 \r，避免用 \0 截断 hbuf（否则后续 Content-Length 解析失效） */

    char method[16] = "", target[520] = "", version[16] = "";
    if (sscanf(hbuf, "%15s %519s %15s", method, target, version) < 2) {
        free(hbuf); closesocket(s); return;
    }
    snprintf(req.method, sizeof req.method, "%s", method);
    /* 拆分 query */
    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        snprintf(req.query, sizeof req.query, "%s", q + 1);
        snprintf(req.target, sizeof req.target, "%s", target);
    } else {
        snprintf(req.target, sizeof req.target, "%s", target);
    }

    /* 解析 Content-Length */
    int body_len = 0;
    {
        const char *cl = strstr(hbuf, "Content-Length:");
        if (!cl) cl = strstr(hbuf, "content-length:");
        if (cl) {
            const char *v = cl + 15;
            while (*v == ' ' || *v == '\t') v++;
            body_len = atoi(v);
        }
    }

    http_resp resp = { 0 };
    if (body_len > 0 && body_len < HDR_CAP) {
        req.body = hbuf + header_end + 4;
        req.body_len = 0;
        /* 读取 body 剩余 */
        int got = total - (header_end + 4);
        while (got < body_len && got < HDR_CAP - 1) {
            int r = recv(s, (char*)req.body + got, body_len - got, 0);
            if (r <= 0) break;
            got += r;
        }
        req.body_len = body_len;
        resp = handler(&req, ctx);
    } else if (body_len > HDR_CAP) {
        resp.status = 413;
        resp.content_type = "text/plain; charset=utf-8";
        resp.body = "payload too large";
        resp.body_len = strlen(resp.body);
        resp.free_body = 0;
    } else {
        resp = handler(&req, ctx);
    }

    /* 组装响应 */
    char head[1024];
    snprintf(head, sizeof head,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "%s\r\n",
             resp.status, status_text(resp.status),
             resp.content_type ? resp.content_type : "application/json; charset=utf-8",
             resp.body_len ? resp.body_len : 0,
             resp.extra_headers ? resp.extra_headers : "");
    http_send_all(s, head, (int)strlen(head));
    if (resp.body && resp.body_len) http_send_all(s, resp.body, (int)resp.body_len);
    if (resp.free_body && resp.body) free((void*)resp.body);
    if (resp.free_extra && resp.extra_headers) free((void*)resp.extra_headers);

    free(hbuf);
    closesocket(s);
}

static DWORD WINAPI conn_thread(LPVOID arg) {
    conn_ctx *c = (conn_ctx*)arg;
    handle_conn(c->s, c->handler, c->ctx);
    free(c);
    return 0;
}

int http_run(const char *host, int port, http_handler handler, void *ctx, volatile bool *running) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { WSACleanup(); return -1; }

    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof opt);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr(host);

    if (bind(ls, (struct sockaddr*)&sa, sizeof sa) == SOCKET_ERROR) {
        closesocket(ls); WSACleanup(); return -1;
    }
    if (listen(ls, 32) == SOCKET_ERROR) {
        closesocket(ls); WSACleanup(); return -1;
    }
    fprintf(stderr, "HTTP listening on %s:%d\n", host, port);

    while (running && *running) {
        SOCKET cs = accept(ls, NULL, NULL);
        if (cs == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR || WSAGetLastError() == WSAECONNABORTED) continue;
            break;
        }
        conn_ctx *c = (conn_ctx*)malloc(sizeof(conn_ctx));
        if (!c) { closesocket(cs); continue; }
        c->s = cs; c->handler = handler; c->ctx = ctx;
        HANDLE hth = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
        if (!hth) { free(c); closesocket(cs); }
        else CloseHandle(hth);
    }
    closesocket(ls);
    WSACleanup();
    return 0;
}