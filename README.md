# mini-redis

用 C 从零实现的最小 Redis 兼容服务端。目标是**与真 Redis 互操作**：可以
直接用 `redis-cli` / `redis-benchmark` 连上来用，命令语义与 Redis 7.0.15
做过逐项对拍对齐。零第三方依赖，关键设计处均有注释与验收证据（内部文档见
[`docs/`](docs/)）。

## 功能特性

### 协议层（RESP2）

- **RESP2 支持**：多 bulk 数组（`*3` / `$len`）与 **inline 命令**
  （`SET foo bar\r\n` 纯文本行，`redis-cli --pipe` 同款）
- **增量式解析器**，半包/粘包安全：数据不齐返回 `NEED_MORE` 等待下一个
  事件，一包多命令逐条执行；解析零拷贝（缓冲内改 `\r` 为 `\0`，
  `argv_len` 保留原始长度）
- **二进制安全**：value 可为任意字节序列（含 `\x00` / `\xff`）
- **命令名大小写不敏感、key 区分大小写**（对齐真 Redis）
- **未知命令干净报错** `-ERR unknown command ...`；`HELLO` 未实现，
  但 redis-cli 6+ 收到错误后自动回落 RESP2，互操作不受影响

### 数据结构

- **自研哈希表**（`dict`）：djb2 + 链地址法，负载因子 >0.75 自动扩容，
  值二进制安全
- **数据表与过期表分离**（对齐真 Redis `db->expires` 设计），删除路径
  两表成对维护

### 命令

| 命令 | 说明 |
|------|------|
| `PING [msg]` | 无参回 `PONG`；带参回显 bulk |
| `SET key value` | 写入；覆盖旧值并**清除该 key 的过期** |
| `GET key` | 读取；缺失/已过期返回 nil |
| `DEL key [key ...]` | 多 key 删除，返回实际删除个数 |
| `EXPIRE key seconds` | 设置过期；`0`/负数立即删除；非整数报错 |
| `TTL key` | 剩余秒数（毫秒四舍五入）；无过期 `-1`，不存在/已过期 `-2`（TTL 触发惰性回收） |
| `ECHO msg` | 原样回显（`redis-cli --pipe` 探活依赖） |

### 过期机制（惰性删除）

GET / EXPIRE / TTL 入口前检查过期表，`expire_at <= now` 即同步删除
数据表 + 过期表记录（stats 里 `expired` 计数）。未过期 key 只多一次哈希
探测，**零定时器**。语义与真 Redis 对齐：TTL 秒数四舍五入、
`EXPIRE k 0` 立即删、`SET` 清 TTL、`EXPIRE` 覆盖旧过期。

### 网络模型

- Linux **epoll 事件循环**，单线程无锁（对齐真 Redis 架构）
- **ET 边沿触发 + 全量排空**：读到 `EAGAIN` 才算处理完，连接保留等待
  下一个事件（`EAGAIN ≠ EOF`，ET 编程的关键陷阱）
- 非阻塞 IO，`write_all` 遇 `EAGAIN` 用 `poll(EPOLLOUT)` 续写

## 快速开始

> 依赖：Linux / WSL2（epoll 为 Linux 专属），`gcc` + `make`；
> 互操作测试另需 `redis-cli`（`sudo apt install -y redis-tools`）。

```bash
make            # 编译 mini-redis（RESP 服务端）
make run        # ./mini-redis 7777
```

## 用法示例

```bash
./mini-redis 7777 &            # 启动服务端（默认端口 7777）
redis-cli -p 7777
```

```text
127.0.0.1:7777> ping
PONG
127.0.0.1:7777> set name mini-redis
OK
127.0.0.1:7777> get name
"mini-redis"
127.0.0.1:7777> expire name 10
(integer) 1
127.0.0.1:7777> ttl name
(integer) 9
127.0.0.1:7777> del name
(integer) 1
```

批量导入（inline 命令 + ECHO 探活，与真 Redis 行为一致）：

```bash
cat data.txt | redis-cli -p 7777 --pipe
```

## 测试

| 命令 | 内容 |
|------|------|
| `make test` | M1 协议（12 用例）+ M2 过期（14 用例）原生 socket 自测 |
| `make test-m0` | M0 epoll echo 压测回归（30 并发 × 40 轮） |
| `bash tests/m1_interop_test.sh` | 真机 redis-cli 互操作验收 |
| `bash tests/m2_interop_test.sh` | redis-cli TTL/过期互操作验收 |
| `bash tests/pre_push_verify.sh` | 公开前一键验收 + 状态快照 |

## 项目结构

```
src/server.c          epoll 事件循环 + 命令分发（7 个命令）
src/resp.c/.h         RESP2 增量解析器 + 回复序列化
src/dict.c/.h         哈希表键空间（djb2 + 链地址法 + 扩容）
src/echo_server.c     M0 底座：可切换 ET/LT 的 echo server（事件循环验证）
tests/                原生 socket 自测（python）+ redis-cli 互操作（shell）
docs/                 内部文档（里程碑验收记录、面试检验清单）
```

## 开发状态

M0–M2 已完成：自测 26/26 全绿，真机 redis-cli 互操作与 Redis 7.0.15 语义
对拍对齐。M3（AOF 持久化）、M4（`redis-benchmark` 对拍）规划中。
详细验收数据见 [`docs/开发里程碑与验收.md`](docs/开发里程碑与验收.md)。

## License

[MIT](LICENSE)