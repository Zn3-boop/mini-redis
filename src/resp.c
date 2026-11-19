/* RESP2 协议：增量解析 + 回复序列化。
 * 增量解析 = 网络数据可能半包/粘包到达，缓冲里攒着，能凑齐一个命令就解析一个。
 */
#include "resp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESP_MAX_ARGC 1024
#define RESP_MAX_BULK (64 * 1024 * 1024)

static int rb_grow(RespBuf *rb, size_t need) {
    if (need <= rb->cap) return 0;
    size_t ncap = rb->cap ? rb->cap * 2 : 4096;
    if (ncap < need) ncap = need;
    char *nb = realloc(rb->buf, ncap);
    if (!nb) return -1;
    rb->buf = nb;
    rb->cap = ncap;
    return 0;
}

int resp_append(RespBuf *rb, const char *data, size_t n) {
    if (rb_grow(rb, rb->len + n) < 0) return -1;
    memcpy(rb->buf + rb->len, data, n);
    rb->len += n;
    return 0;
}

void resp_consume(RespBuf *rb, size_t consumed) {
    if (consumed >= rb->len) {
        rb->len = 0;
        return;
    }
    memmove(rb->buf, rb->buf + consumed, rb->len - consumed);
    rb->len -= consumed;
}

void resp_free(RespBuf *rb) {
    free(rb->buf);
    rb->buf = NULL;
    rb->len = rb->cap = 0;
}

/* 从 pos 起找 \r\n，找到则 *n = 行内容长度并返回 0；未找到返回 1 */
static int scan_crlf(const RespBuf *rb, size_t pos, size_t *n) {
    for (size_t i = pos; i + 1 < rb->len; i++) {
        if (rb->buf[i] == '\r' && rb->buf[i + 1] == '\n') {
            *n = i - pos;
            return 0;
        }
    }
    return 1;
}

static int parse_ll(const char *s, size_t n, long *out) {
    if (n == 0) return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
        if (v < 0) return -1; /* 溢出 */
    }
    *out = v;
    return 0;
}

/* inline 命令格式：一行纯文本，空白分隔，支持 '...' / "..." 引号。
 * 真 Redis 的 processInlineBuffer 同款语义；redis-cli --pipe 发的正是这个。
 * 返回：RESP_OK（argc 可能为 0 = 空行，调用方跳过）、RESP_NEED_MORE、RESP_PROTOCOL_ERROR。 */
static int resp_parse_inline(RespBuf *rb, RespCmd *cmd) {
    size_t ln;
    if (scan_crlf(rb, 0, &ln) != 0) return RESP_NEED_MORE;

    size_t i = 0;
    while (i < ln && (rb->buf[i] == ' ' || rb->buf[i] == '\t')) i++;
    if (i == ln) {
        /* 空行：消费掉，argc=0 由调用方跳过 */
        cmd->consumed = ln + 2;
        return RESP_OK;
    }

    size_t tok_start[RESP_MAX_ARGC], tok_end[RESP_MAX_ARGC];
    int argc = 0;
    while (i < ln && argc < RESP_MAX_ARGC) {
        while (i < ln && (rb->buf[i] == ' ' || rb->buf[i] == '\t')) i++;
        if (i >= ln) break;
        char q = 0;
        if (rb->buf[i] == '\'' || rb->buf[i] == '"') {
            q = rb->buf[i];
            i++;
        }
        size_t start = i;
        if (q) {
            while (i < ln && rb->buf[i] != q) i++;
            if (i >= ln) return RESP_PROTOCOL_ERROR; /* 引号未闭合 */
            tok_start[argc] = start;
            tok_end[argc] = i; /* 闭引号位置，改 \0 */
            i++;
            if (i < ln && rb->buf[i] != ' ' && rb->buf[i] != '\t')
                return RESP_PROTOCOL_ERROR; /* "ab"c 非法 */
        } else {
            while (i < ln && rb->buf[i] != ' ' && rb->buf[i] != '\t') i++;
            tok_start[argc] = start;
            tok_end[argc] = i; /* 分隔空白位置，改 \0 */
        }
        argc++;
        if (argc == RESP_MAX_ARGC && i < ln)
            return RESP_PROTOCOL_ERROR; /* 参数超限 */
    }

    cmd->argv = malloc(sizeof(char *) * (size_t)argc);
    cmd->argv_len = malloc(sizeof(size_t) * (size_t)argc);
    if (!cmd->argv || !cmd->argv_len) {
        free(cmd->argv);
        free(cmd->argv_len);
        return RESP_PROTOCOL_ERROR;
    }
    for (int k = 0; k < argc; k++) {
        cmd->argv[k] = rb->buf + tok_start[k];
        cmd->argv_len[k] = tok_end[k] - tok_start[k];
        rb->buf[tok_end[k]] = '\0'; /* 分隔符改 NUL，argv 零拷贝 NUL 结尾 */
    }
    cmd->argc = (size_t)argc;
    cmd->consumed = ln + 2;
    return RESP_OK;
}

int resp_parse(RespBuf *rb, RespCmd *cmd) {
    cmd->argv = NULL;
    cmd->argv_len = NULL;
    cmd->argc = 0;
    cmd->consumed = 0;

    if (rb->len < 1) return RESP_NEED_MORE;
    if (rb->buf[0] != '*') return resp_parse_inline(rb, cmd);

    size_t pos = 1, ln = 0;
    if (scan_crlf(rb, pos, &ln) != 0) return RESP_NEED_MORE;
    long argc;
    if (parse_ll(rb->buf + pos, ln, &argc) != 0 || argc <= 0 || argc > RESP_MAX_ARGC)
        return RESP_PROTOCOL_ERROR;
    pos += ln + 2;

    /* se[i*2]   = 第 i 个参数的 payload 起点
     * se[i*2+1] = 第 i 个参数 payload 结尾的 '\r' 位置 */
    size_t *se = malloc(sizeof(size_t) * (size_t)argc * 2);
    if (!se) return RESP_PROTOCOL_ERROR;

    for (long i = 0; i < argc; i++) {
        if (rb->len <= pos || rb->buf[pos] != '$') {
            free(se);
            return RESP_PROTOCOL_ERROR;
        }
        pos++;
        size_t bl;
        if (scan_crlf(rb, pos, &bl) != 0) {
            free(se);
            return RESP_NEED_MORE;
        }
        long blen;
        if (parse_ll(rb->buf + pos, bl, &blen) != 0 || blen < 0 || blen > RESP_MAX_BULK) {
            free(se);
            return RESP_PROTOCOL_ERROR;
        }
        pos += bl + 2;
        if (rb->len < pos + (size_t)blen + 2) {
            free(se);
            return RESP_NEED_MORE;
        }
        if (rb->buf[pos + blen] != '\r' || rb->buf[pos + blen + 1] != '\n') {
            free(se);
            return RESP_PROTOCOL_ERROR;
        }
        se[i * 2] = pos;
        se[i * 2 + 1] = pos + blen;
        pos += (size_t)blen + 2;
    }

    /* 整个命令已完整：把每个 payload 结尾的 '\r' 改写为 '\0'，
     * argv 直接指向缓冲内的 NUL 结尾字符串（零拷贝）。 */
    cmd->argv = malloc(sizeof(char *) * (size_t)argc);
    cmd->argv_len = malloc(sizeof(size_t) * (size_t)argc);
    if (!cmd->argv || !cmd->argv_len) {
        free(cmd->argv);
        free(cmd->argv_len);
        free(se);
        return RESP_PROTOCOL_ERROR;
    }
    for (long i = 0; i < argc; i++) {
        cmd->argv[i] = rb->buf + se[i * 2];
        cmd->argv_len[i] = se[i * 2 + 1] - se[i * 2];
        rb->buf[se[i * 2 + 1]] = '\0';
    }
    cmd->argc = (size_t)argc;
    cmd->consumed = pos;
    free(se);
    return RESP_OK;
}

/* ---- 回复序列化 ---- */

void resp_out_init(RespOut *o) {
    o->buf = NULL;
    o->len = o->cap = 0;
}

static int out_grow(RespOut *o, size_t need) {
    if (need <= o->cap) return 0;
    size_t ncap = o->cap ? o->cap * 2 : 256;
    if (ncap < need) ncap = need;
    char *nb = realloc(o->buf, ncap);
    if (!nb) return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

static void out_put(RespOut *o, const char *s, size_t n) {
    if (out_grow(o, o->len + n) < 0) return;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

void resp_out_simple(RespOut *o, const char *s) {
    out_put(o, "+", 1);
    out_put(o, s, strlen(s));
    out_put(o, "\r\n", 2);
}

void resp_out_bulk(RespOut *o, const char *s, size_t n) {
    char hdr[32];
    int hn = snprintf(hdr, sizeof hdr, "$%zu\r\n", n);
    out_put(o, hdr, (size_t)hn);
    out_put(o, s, n);
    out_put(o, "\r\n", 2);
}

void resp_out_error(RespOut *o, const char *msg) {
    out_put(o, "-", 1);
    out_put(o, msg, strlen(msg));
    out_put(o, "\r\n", 2);
}

void resp_out_nil(RespOut *o) { out_put(o, "$-1\r\n", 5); }

void resp_out_free(RespOut *o) {
    free(o->buf);
    o->buf = NULL;
    o->len = o->cap = 0;
}