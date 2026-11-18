/*
 * mini-redis — M0: epoll TCP echo server
 *
 * 单一职责：验证 Linux 专属的 epoll 事件循环底座。M1 的 RESP 协议层将直接
 * 挂在这个循环上，因此本文件刻意保持单文件、零外部依赖，只做一件事：
 * 「非阻塞 IO + 事件循环 + 回显」。
 *
 * 两种触发模式（第二个参数切换，默认 et）：
 *   et  edge-triggered : 内核只在状态变化时通知一次；处理方必须把数据一直
 *       读到 EAGAIN，否则剩余数据要等下一次新事件才被处理。唤醒次数最少，
 *       代价是必须写"读到 EAGAIN"的排空循环。
 *   lt  level-triggered: 只要缓冲区还有未读数据，每次 epoll_wait 都会再次
 *       通知；不容易漏读、实现直觉，但同样数据量下用户态唤醒更多。
 *
 * 用法:  ./echo-server [port=7777] [et|lt]
 * 退出:  Ctrl-C 打印统计后退出
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT   7777
#define MAX_EVENTS     1024
#define BUF_SIZE       65536
#define LISTEN_BACKLOG 512

static volatile sig_atomic_t g_stop = 0;
static long      g_clients = 0;   /* 已服务客户端数 */
static long long g_bytes   = 0;   /* 已回显字节数   */
static bool      g_edge    = true; /* 默认 ET      */

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_listener(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lfd);
        return -1;
    }
    if (listen(lfd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(lfd);
        return -1;
    }
    if (set_nonblock(lfd) < 0) {
        perror("fcntl(listener)");
        close(lfd);
        return -1;
    }
    return lfd;
}

/* 非阻塞 fd 上的全量写出；EAGAIN 时用 poll 等 EPOLLOUT 再续写 */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, -1);
            if (pr <= 0) return -1;
            continue;
        }
        return -1; /* 对端关闭或硬错误 */
    }
    return 0;
}

/*
 * 单连接回读 + 原样回显。
 * 无论 ET/LT 都执行"读到 EAGAIN"排空循环：ET 下这是硬性要求（漏读=丢数据），
 * LT 下多读是冗余但无害；两者共享同一份处理逻辑，差异只在 epoll 注册标志。
 */
static void handle_peer(int epfd, int cfd) {
    static char buf[BUF_SIZE];
    for (;;) {
        ssize_t n = read(cfd, buf, sizeof(buf));
        if (n > 0) {
            g_bytes += n;
            if (write_all(cfd, buf, (size_t)n) < 0) goto out;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; /* 排空完成，连接保留 */
            goto out; /* 硬错误，放弃连接 */
        }
        goto out; /* n == 0: 对端关闭 */
    }
out:
    epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL); /* 先摘除再 close，避免 fd 复用串线 */
    close(cfd);
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [port=7777] [et|lt]\n", argv0);
    return 1;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        char *end = NULL;
        long p = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || p < 1 || p > 65535) return usage(argv[0]);
        port = (int)p;
    }
    if (argc > 2) {
        if (strcmp(argv[2], "et") == 0) g_edge = true;
        else if (strcmp(argv[2], "lt") == 0) g_edge = false;
        else return usage(argv[0]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int lfd = make_listener(port);
    if (lfd < 0) return 1;

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); close(lfd); return 1; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.fd = lfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        perror("epoll_ctl(listener)");
        close(epfd); close(lfd); return 1;
    }

    fprintf(stderr, "echo-server %s on port %d (pid %d)\n",
            g_edge ? "ET" : "LT", port, (int)getpid());

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == lfd) {
                /* 监听 fd 就绪：把 accept 队列排空（ET 要求，LT 冗余无害） */
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept(lfd, (struct sockaddr *)&caddr, &clen);
                    if (cfd < 0) {
                        if (errno == EINTR) continue;
                        break; /* EAGAIN 或硬错误，退出 accept 循环 */
                    }
                    if (set_nonblock(cfd) < 0 ||
                        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd,
                                  &(struct epoll_event){
                                      .events = EPOLLIN | (g_edge ? EPOLLET : 0),
                                      .data.fd = cfd }) < 0) {
                        close(cfd);
                        continue;
                    }
                    g_clients++;
                }
                continue;
            }

            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                /* 先尝试读净剩余数据，再收尾 */
                handle_peer(epfd, fd);
                continue;
            }
            if (evs & EPOLLIN) {
                handle_peer(epfd, fd);
            }
        }
    }

    close(epfd);
    close(lfd);
    fprintf(stderr, "stats: mode=%s clients=%ld echoed_bytes=%lld\n",
            g_edge ? "et" : "lt", g_clients, g_bytes);
    return 0;
}