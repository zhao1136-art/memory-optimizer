#ifndef JSON_H
#define JSON_H

#include "memtool.h"

/* 轻量 JSON 序列化器（栈式，自动处理 , 与引号转义） */
typedef struct json {
    strbuf s;
    int depth;
    unsigned char beg[64];       /* 1=object, 0=array */
    unsigned char hasval[64];    /* 本层是否已含成员 */
    unsigned char keyd[64];      /* 刚写 key，等待其值（值函数应跳过分隔符） */
} json;

void json_init(json *j, size_t cap);
void json_dtor(json *j);
const char *json_cstr(json *j);  /* NUL 结尾 json 字符串 */
size_t json_len(const json *j);

void j_begin_obj(json *j);
void j_begin_arr(json *j);
void j_end(json *j);             /* 结束后一个 obj/arr */
void j_key(json *j, const char *k);
void j_str(json *j, const char *v);
void j_strn(json *j, const char *v, size_t n);
void j_int(json *j, int64_t v);
void j_uint(json *j, uint64_t v);
void j_dbl(json *j, double v);
void j_bool(json *j, int v);
void j_null(json *j);

/* 极简解析：从 JSON 字符串中取 "key" 的整数/字符串（供 POST body） */
int json_get_int(const char *js, const char *key, int64_t *out, int *found);
int json_get_str(const char *js, const char *key, char *out, size_t cap, int *found);

#endif