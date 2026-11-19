/* mini-redis — M1: 在 M0 的 epoll 底座上叠加 RESP2 协议层。
 * 命令：PING / SET / GET（大小写不敏感）；
 * 未知命令回 -ERR（redis-cli 6+ 会先发 HELLO 做 RESP3 握手，
 * 收到这个 error 后自动回落 RESP2——本文件的互操作关键路径）。
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dict.h"
#include "resp.h"

#define DEFAULT_PORT   7777
#define MAX_EVENTS     1024
#define READ_CHUNK     8192
#define LISTEN_BACKLOG 512

/* 键空间：数据表 + 独立过期表（对齐真 Redis db->expires 设计）。
 * g_exp 的 value 是绝对过期时刻（ms）的十进制字符串。 */
static Dict g_db;
static Dict g_exp;
static volatile sig_atomic_t g_stop = 0;
static long g_clients = 0;
static long long g_commands = 0;
static long long g_expired = 0; /* 惰性过期实际删除的 key 数（stats 用） */

typedef struct Conn {
    int fd;
    RespBuf rb;
    int dead;
} Conn;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int make_listener(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); close(lfd); return -1; }
    if (listen(lfd, LISTEN_BACKLOG) < 0) { perror("listen"); close(lfd); return -1; }
    if (set_nonblock(lfd) < 0) { perror("fcntl"); close(lfd); return -1; }
    return lfd;
}

/* 非阻塞 fd 上全量写出；EAGAIN 时 poll 等 EPOLLOUT 再续写。
 * M1 正确性优先：回复都很小，同步写可接受；M4 性能对比时再换输出缓冲。 */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, -1) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void conn_close(int epfd, Conn *c) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    resp_free(&c->rb);
    free(c);
}

/* === M2: 过期元数据 + 惰性过期 === */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 严格数字参数：strtoll 且必须整体消费，否则 -1（对齐 Redis "not an integer" 报错） */
static int parse_ll_arg(const char *s, long long *out) {
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

static void set_expire(const char *key, long long at_ms) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", at_ms);
    dict_set(&g_exp, key, buf, (size_t)n);
}

/* 返回 1 = 已过期并删除（数据表+过期表成对清理）；0 = 有效或不存在 */
static int expire_if_needed(const char *key, long long now) {
    DbValue v;
    if (dict_get(&g_exp, key, &v) != 0) return 0;
    long long at = strtoll(v.data, NULL, 10);
    if (at > now) return 0;
    dict_del(&g_db, key);
    dict_del(&g_exp, key);
    g_expired++;
    return 1;
}

static void dispatch(RespCmd *cmd, RespOut *out) {
    const char *name = (cmd->argc > 0) ? cmd->argv[0] : "";
    if (strcasecmp(name, "PING") == 0) {
        if (cmd->argc == 1) {
            resp_out_simple(out, "PONG");
        } else if (cmd->argc == 2) {
            resp_out_bulk(out, cmd->argv[1], cmd->argv_len[1]);
        } else {
            resp_out_error(out, "ERR wrong number of arguments for 'ping' command");
        }
    } else if (strcasecmp(name, "SET") == 0) {
        if (cmd->argc != 3) {
            resp_out_error(out, "ERR wrong number of arguments for 'set' command");
            return;
        }
        if (dict_set(&g_db, cmd->argv[1], cmd->argv[2], cmd->argv_len[2]) == 0) {
            /* SET 覆盖旧值并清除该 key 的过期（真 Redis 语义） */
            dict_del(&g_exp, cmd->argv[1]);
            resp_out_simple(out, "OK");
        } else {
            resp_out_error(out, "ERR out of memory");
        }
    } else if (strcasecmp(name, "GET") == 0) {
        if (cmd->argc != 2) {
            resp_out_error(out, "ERR wrong number of arguments for 'get' command");
            return;
        }
        long long now = now_ms();
        if (expire_if_needed(cmd->argv[1], now)) {
            resp_out_nil(out);
            return;
        }
        DbValue v;
        if (dict_get(&g_db, cmd->argv[1], &v) == 0)
            resp_out_bulk(out, v.data, v.len);
        else
            resp_out_nil(out);
    } else if (strcasecmp(name, "DEL") == 0) {
        if (cmd->argc < 2) {
            resp_out_error(out, "ERR wrong number of arguments for 'del' command");
            return;
        }
        long long deleted = 0;
        for (size_t i = 1; i < cmd->argc; i++) {
            if (dict_del(&g_db, cmd->argv[i]) == 0) {
                dict_del(&g_exp, cmd->argv[i]);
                deleted++;
            }
        }
        resp_out_integer(out, deleted);
    } else if (strcasecmp(name, "EXPIRE") == 0) {
        if (cmd->argc != 3) {
            resp_out_error(out, "ERR wrong number of arguments for 'expire' command");
            return;
        }
        long long seconds;
        if (parse_ll_arg(cmd->argv[2], &seconds) != 0) {
            resp_out_error(out, "ERR value is not an integer or out of range");
            return;
        }
        long long now = now_ms();
        expire_if_needed(cmd->argv[1], now);   /* 先回收已过期残留，key 不存在才回 0 */
        if (dict_get(&g_db, cmd->argv[1], NULL) != 0) {
            resp_out_integer(out, 0);
            return;
        }
        if (seconds <= 0) {                    /* EXPIRE k 0/负数 = 立即删除 */
            dict_del(&g_db, cmd->argv[1]);
            dict_del(&g_exp, cmd->argv[1]);
            resp_out_integer(out, 1);
            return;
        }
        set_expire(cmd->argv[1], now + seconds * 1000);
        resp_out_integer(out, 1);
    } else if (strcasecmp(name, "TTL") == 0) {
        if (cmd->argc != 2) {
            resp_out_error(out, "ERR wrong number of arguments for 'ttl' command");
            return;
        }
        long long now = now_ms();
        expire_if_needed(cmd->argv[1], now);   /* TTL 也会触发惰性删除 */
        if (dict_get(&g_db, cmd->argv[1], NULL) != 0) {
            resp_out_integer(out, -2);
            return;
        }
        DbValue v;
        if (dict_get(&g_exp, cmd->argv[1], &v) != 0) {
            resp_out_integer(out, -1);         /* 无过期设置 */
            return;
        }
        long long rem = strtoll(v.data, NULL, 10) - now;
        if (rem <= 0) {
            dict_del(&g_db, cmd->argv[1]);
            dict_del(&g_exp, cmd->argv[1]);
            g_expired++;
            resp_out_integer(out, -2);
            return;
        }
        resp_out_integer(out, (rem + 500) / 1000); /* 毫秒→秒，四舍五入（真 Redis 同款） */
    } else if (strcasecmp(name, "ECHO") == 0) {
        /* redis-cli --pipe 的探活依赖 ECHO 原样回显 token */
        if (cmd->argc != 2) {
            resp_out_error(out, "ERR wrong number of arguments for 'echo' command");
            return;
        }
        resp_out_bulk(out, cmd->argv[1], cmd->argv_len[1]);
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "ERR unknown command '%s'", name);
        resp_out_error(out, msg);
    }
}

/* === PART2: handle_conn + main 事件循环 === */

/* 读取网络数据 → 追加进连接缓冲 → 增量解析 → 逐命令分发回复。
 * ET 语义：读到 EAGAIN 才算排空；缓冲里没凑齐的命令等下一个事件。 */
static void handle_conn(int epfd, Conn *c) {
    char buf[READ_CHUNK];
    ssize_t n = read(c->fd, buf, sizeof buf);
    if (n > 0) {
        if (resp_append(&c->rb, buf, (size_t)n) < 0) {
            c->dead = 1;
            return;
        }
        for (;;) {
            RespCmd cmd;
            int r = resp_parse(&c->rb, &cmd);
            if (r == RESP_NEED_MORE) return;          /* 半包：等下一次事件 */
            if (r == RESP_PROTOCOL_ERROR) { c->dead = 1; return; }
            if (cmd.argc == 0) {                      /* inline 空行：跳过 */
                free(cmd.argv);
                free(cmd.argv_len);
                resp_consume(&c->rb, cmd.consumed);
                continue;
            }
            g_commands++;
            RespOut out;
            resp_out_init(&out);
            dispatch(&cmd, &out);
            if (write_all(c->fd, out.buf, out.len) < 0) {
                c->dead = 1;
                resp_out_free(&out);
                free(cmd.argv);
                free(cmd.argv_len);
                return;
            }
            resp_out_free(&out);
            free(cmd.argv);
            free(cmd.argv_len);
            resp_consume(&c->rb, cmd.consumed);
        }
    }
    if (n == 0) { c->dead = 1; return; }              /* 对端关闭 */
    if (errno == EINTR) return;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return; /* 排空完成，连接保留 */
    c->dead = 1;                                      /* 硬错误 */
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        char *end = NULL;
        long p = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || p < 1 || p > 65535) {
            fprintf(stderr, "usage: %s [port=%d]\n", argv[0], DEFAULT_PORT);
            return 1;
        }
        port = (int)p;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    dict_init(&g_db);
    dict_init(&g_exp);

    int lfd = make_listener(port);
    if (lfd < 0) return 1;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); close(lfd); return 1; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) { perror("epoll_ctl"); close(lfd); return 1; }

    fprintf(stderr, "mini-redis %s listening on %d (pid %d)\n",
            "RESP2", port, (int)getpid());

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;
            if (fd == lfd) {
                /* accept 队列排空（ET 要求） */
                for (;;) {
                    struct sockaddr_in ca;
                    socklen_t cl = sizeof ca;
                    int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
                    if (cfd < 0) {
                        if (errno == EINTR) continue;
                        break; /* EAGAIN 或硬错误 */
                    }
                    Conn *c = calloc(1, sizeof *c);
                    if (!c || set_nonblock(cfd) < 0) {
                        close(cfd);
                        free(c);
                        continue;
                    }
                    c->fd = cfd;
                    struct epoll_event ce = { .events = EPOLLIN | EPOLLET, .data.ptr = c };
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ce) < 0) {
                        close(cfd);
                        free(c);
                        continue;
                    }
                    g_clients++;
                }
                continue;
            }
            Conn *c = events[i].data.ptr;
            if (evs & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                handle_conn(epfd, c);
            }
            if (c->dead) conn_close(epfd, c);
        }
    }

    close(epfd);
    close(lfd);
    fprintf(stderr, "stats: clients=%ld commands=%lld expired=%lld\n",
            g_clients, g_commands, g_expired);
    dict_free(&g_exp);
    dict_free(&g_db);
    return 0;
}