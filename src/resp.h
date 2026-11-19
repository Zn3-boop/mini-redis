#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include <stddef.h>

/* ---- 增量式 RESP2 命令解析 ---- */

typedef struct {
    char  *buf;   /* 累积缓冲 */
    size_t len;   /* 已填充字节 */
    size_t cap;
} RespBuf;

typedef struct {
    char  **argv;     /* 指向 RespBuf 内部存储，下一次 append 前有效（NUL 结尾） */
    size_t *argv_len; /* 各参数原始长度（二进制安全用，argv 被 NUL 截断不影响它） */
    size_t argc;
    size_t consumed;  /* 本命令在缓冲中占用的字节数，供 resp_consume */
} RespCmd;

enum { RESP_OK = 0, RESP_NEED_MORE = 1, RESP_PROTOCOL_ERROR = -1 };

int  resp_append(RespBuf *rb, const char *data, size_t n); /* 追加网络数据 */
int  resp_parse(RespBuf *rb, RespCmd *cmd);                /* 尝试解析一个命令 */
void resp_consume(RespBuf *rb, size_t consumed);           /* 丢弃已消费字节 */
void resp_free(RespBuf *rb);

/* ---- 回复序列化（写入动态缓冲） ---- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} RespOut;

void resp_out_init(RespOut *o);
void resp_out_simple(RespOut *o, const char *s);              /* +s\r\n    */
void resp_out_bulk(RespOut *o, const char *s, size_t n);      /* $n\r\ns\r\n */
void resp_out_error(RespOut *o, const char *msg);             /* -msg\r\n  */
void resp_out_nil(RespOut *o);                                /* $-1\r\n   */
void resp_out_free(RespOut *o);

#endif