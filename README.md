# mini-redis

用 C 从零实现一个兼容 `redis-cli` / `redis-benchmark` 的迷你 Redis。
独立仓库，只服务一个目标：**把面试八股里的一半题从「我背过」变成「我写过」**。

> 与 d:\ecv / d:\ecv-paypal 无运行期依赖。两者均为 Python(FastAPI) 技术栈且无公网端点；
> 本仓库的合法接缝是未来 d:\ecv 公网化时 `rate_limit.py` 的 Redis 后端升级（届时用真 Redis）。

## 技术前提

- Linux 专属 `epoll` 事件循环 → 在 **WSL2 (Ubuntu 24.04)** 中构建运行
- `gcc 13` + `make` + `python3`（测试脚本用）
- M1 起需要 `redis-cli` 做互操作验收；M4 需要 `redis-server` 做基准对照组
  （`sudo apt install -y redis-tools redis-server`）

## 里程碑（按可演示证据验收，不按时间）

| 里程碑 | 产出 | 硬验收 | 状态 |
|---|---|---|---|
| M0 | epoll TCP echo server | 多客户端并发压测不断链；能讲清 ET/LT | ✅ 见下 |
| M1 | RESP2 解析器 + PING/SET/GET | 真机 redis-cli 互操作；处理 redis-cli 6+ 的 HELLO 回落 | ⬜ |
| M2 | 哈希表键空间 + DEL/EXPIRE/TTL 惰性过期 | redis-cli 验证 TTL 递减、过期返回 nil | ⬜ |
| M3 | AOF 追加 + 启动回放 | SET 后 kill -9 重启，GET 读回一致 | ⬜ |
| M4 | redis-benchmark 对拍同机 redis-server | QPS/P99 对比表 + README 取舍记录 | ⬜ |
| M5 | 可选：玩具主从 或 SETNX+过期锁 | 不承诺 | ⬜ |

## 构建与运行

```bash
make            # 编译 ./echo-server
make run        # 默认端口 7777，ET 模式
./echo-server 7777 lt   # LT 模式
make test       # M0 验收：30 并发 x 40 轮 + 8 x 64KB 大包，逐字节核对
```

## M0：epoll TCP echo server

### ET 与 LT 的区别（本项目里的证据）

epoll 通知语义分两种，差在**通知次数**而不是能否读到数据：

- **LT（水平触发）**：只要读缓冲区还有未读数据，`epoll_wait` 就会反复通知。
  实现直觉、不易漏读，但同等数据量下用户态唤醒次数更多。
- **ET（边沿触发）**：只在"无→有"状态跳变时通知一次。处理方必须把数据读到
  `EAGAIN`（排空循环），否则剩余数据要等下一次新事件——代码更易错，
  但唤醒次数最少，是 Redis 单线程事件循环高并发的关键之一。

本项目 ET/LT 共享同一套"排空循环"逻辑（见 `src/echo_server.c:handle_peer`），
差异只体现在 `epoll_ctl` 注册的 `EPOLLET` 标志上：
ET 下排空是硬性要求（漏读=丢数据），LT 下是冗余但无害。
这也解释了为什么 Redis 选择 ET + 全量排空：**用确定性换唤醒次数**。

### 验收证据（M0）

2026-11-18，WSL2 Ubuntu 24.04，gcc 13.3：

| 模式 | 结果 | 耗时 | 回显字节 |
|---|---|---|---|
| ET | PASS 1208/1208，FAIL 0 | 0.31s | 2,298,128 |
| LT | PASS 1208/1208，FAIL 0 | 0.34s | 2,298,128 |

压测构成：30 并发客户端 × 40 轮（16/256/4096B 随机 payload，每轮逐字节核对）+ 8 × 64KB 大包。

**过程中的真 bug（比代码更值钱的面试素材）**：首版把「读到 EAGAIN 排空完成」误判为「对端关闭」，
直接 close 连接，导致 38/1208 失败、所有连接耗尽即断。这正是 ET 编程的经典陷阱——
**EAGAIN ≠ EOF**：排空后必须保留连接等待下一个事件，只有 read 返回 0（或硬错误）才允许关闭。
修掉后双模式全绿。这条记录证明踩过坑、知道坑的形状，比"我会 epoll"可信一个量级。

**ET vs LT 实测观感**：相同压测下功能全绿，ET 略快（0.31s vs 0.34s）。差异不显著是预期——
echo 是轻负载，ET 的"少唤醒"优势要在"万级空闲连接 + 大量短请求"场景才放大，这正是 Redis
单线程事件循环选 ET 的语境。README 的取舍记录到 M4 会用 redis-benchmark 数据补全。

## 知识地图（覆盖边界）

- 写过级（本仓库覆盖）：IO 多路复用、RESP、哈希表/跳表、AOF 追加回放、惰性过期
- 读过级（面试必备，官方文档 + 源码注释）：穿透/击穿/雪崩、分布式锁与 Redlock、
  RDB vs AOF vs 混合、主从/哨兵/Cluster、SDS vs C 字符串、bigkey/热 key