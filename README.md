# mini-redis

用 C 从零实现一个兼容 `redis-cli` / `redis-benchmark` 的迷你 Redis。
目标不是复制功能，而是把面试八股变成"我写过、测过、和真 Redis 对拍过"。

> 里程碑制推进（M0→M4），每个里程碑有独立验收证据：并发压测、
> 真机 redis-cli 互操作、与 Redis 7.0.15 语义对拍、原生 socket 自测。

## 技术前提

- Linux 专属 `epoll` 事件循环 → 在 **WSL2 (Ubuntu 24.04)** 中构建运行
- `gcc 13` + `make` + `python3`（测试脚本用）
- M1 起需要 `redis-cli` 做互操作验收；M4 需要 `redis-server` 做基准对照组
  （`sudo apt install -y redis-tools redis-server`）

## 里程碑（按可演示证据验收，不按时间）

| 里程碑 | 产出 | 硬验收 | 状态 |
|---|---|---|---|
| M0 | epoll TCP echo server | 多客户端并发压测不断链；能讲清 ET/LT | ✅ 见下 |
| M1 | RESP2 解析器 + PING/SET/GET/ECHO | 真机 redis-cli 互操作；处理 redis-cli 6+ 的 HELLO 回落 | ✅ 互操作对齐真 Redis：`errors: 0, replies: 3` |
| M2 | 哈希表键空间 + DEL/EXPIRE/TTL 惰性过期 | redis-cli 验证 TTL 递减、过期返回 nil | ✅ 互操作 + 真 Redis 语义对拍对齐：TTL -2/-1、EXPIRE 0 立即删、SET 清 TTL |
| M3 | AOF 追加 + 启动回放 | SET 后 kill -9 重启，GET 读回一致 | ⬜ |
| M4 | redis-benchmark 对拍同机 redis-server | QPS/P99 对比表 + README 取舍记录 | ⬜ |
| M5 | 可选：玩具主从 或 SETNX+过期锁 | 不承诺 | ⬜ |

## 构建与运行

```bash
make            # 编译 mini-redis（M1/M2 主体）+ echo-server（M0）
make run        # ./mini-redis 7777
make test       # M1（12 用例）+ M2（14 用例）原生 socket 验收
make test-m0    # M0 回归压测
bash tests/m1_interop_test.sh   # M1 redis-cli 互操作
bash tests/m2_interop_test.sh   # M2 redis-cli 互操作
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

## M1：RESP2 协议层 + PING/SET/GET/ECHO（✅ 完成）

### 结构

- `src/resp.c/.h` — 增量式 RESP2 命令解析（NEED_MORE 处理半包/粘包）+ 回复序列化
  - 支持 **RESP 数组**（redis-cli 交互模式）
  - 支持 **inline 命令**（`SET a x\r\n` 纯文本行，真 Redis processInlineBuffer 同款；
    `redis-cli --pipe` 发的正是这个）
  - 空行跳过、引号 `'...'`/`"..."` 切分、二进制安全（argv_len 与 NUL 结尾并存）
- `src/dict.c/.h` — 键空间哈希表（djb2 + 链地址法 + 负载>0.75 扩容），值二进制安全
- `src/server.c` — M0 的 epoll 底座上挂协议层，命令分发大小写不敏感

### 互操作硬证据（redis-cli 7.0.15 真机，mini-redis vs 真 Redis 输出逐字对齐）

```
== 6. --pipe 批量 RESP ==
All data transferred. Waiting for the last reply...
Last reply received from server.
errors: 0, replies: 3
```

HELLO 回落路径（redis-cli 6+ 先发 `HELLO 3` 做 RESP3 握手，server 回
`-ERR unknown command 'HELLO'` 后自动回落 RESP2）✅，未实现命令干净报错不崩 ✅，
`--pipe` 批量（inline + ECHO 探活）✅，`(nil)` 缺失语义 ✅。

### 协议调试实例（面试少有的"我抓过 Redis 的包"素材）

**现象**：`redis-cli --pipe` 批量 SET 后 key 不落库、输出 `No replies for 30
seconds`。**定位**：用 nc 假服务器抓 redis-cli 发出的字节，再用 Python 中间人
（MITM）对拍真 Redis，发现 `--pipe` 在转发完 inline 命令后，会**自附加一条
`ECHO <随机token>` 探活命令**；真 Redis 原样回显 token 所以计数正常，而我们当时
没实现 ECHO → 回 `-ERR` → 等不到 token 超时。**修复**：实现 ECHO + inline 支持。
这类"用什么证据链定位一个问题"的过程，比"我写过 RESP 解析器"在面试里可信一个量级。

### 原生 socket 自测（2026-11-18，WSL2 / gcc 13.3）

```
RESULT: 12/12 passed
  PASS  PING -> +PONG
  PASS  PING arg -> bulk
  PASS  SET/GET roundtrip
  PASS  GET missing -> nil
  PASS  binary-safe value (\x00\xff)
  PASS  case-insensitive cmd, case-sensitive key
  PASS  HELLO -> ERR (redis-cli 6+ 回落路径)
  PASS  wrong arg count -> ERR
  PASS  pipeline 3-in-1 packet
  PASS  ECHO -> 原样回显（redis-cli --pipe 探活依赖）
  PASS  half packet (增量解析 NEED_MORE)
  PASS  30 concurrent x 20 ops
```

过程小坑：最初想当然"set K v 后 get k 能取到"——错，**Redis 命令名大小写不敏感、
key 敏感**；对齐语义后测试才全绿。

### 运行

```bash
make          # 编译 mini-redis（M1）+ echo-server（M0）
./mini-redis 7777
make test     # M1 原生 socket 验收（12 用例）
bash tests/m1_interop_test.sh   # M1 redis-cli 互操作验收（需 redis-tools）
make test-m0  # 回归 M0 压测
```

## M2：DEL/EXPIRE/TTL + 惰性过期（✅ 完成）

### 设计：数据与过期元数据分离（对齐真 Redis `db->expires`）

- `g_db`（Dict）：key → value，纯数据表
- `g_exp`（复用同一 Dict）：key → 绝对过期时刻（毫秒，十进制字符串）
- 删除路径全部**两表成对**：`expire_if_needed`（惰性过期）、`DEL`、`SET` 覆盖清 TTL

惰性过期与真 Redis 同款触发点（`lookupKeyRead/lookupKeyWrite → expireIfNeeded`）：
GET / EXPIRE / TTL 进入前先查过期表，`expire_at <= now` 则同步删除两表记录
（`g_expired` 计数器进启动 stats），再继续走正常逻辑。未过期 key 只多一次哈希表
探测的开销，零定时器。

### 命令语义（与真 Redis 7.0.15 逐项对拍对齐）

| 场景 | 返回 | 对齐 |
|---|---|---|
| `DEL` 命中/未命中/多 key | 1 / 0 / 命中数 | ✅ |
| `EXPIRE` key 存在 / 不存在 | 1 / 0 | ✅ |
| `EXPIRE k 0` 或负数（key 存在） | 立即删除，1 | ✅ |
| `EXPIRE` 非数字 | `ERR value is not an integer or out of range` | ✅ |
| `TTL` 无过期 / 不存在 / 已过期 | -1 / -2 / -2（触发回收） | ✅ |
| `TTL` 剩余 | 毫秒四舍五入 `(rem+500)/1000` | ✅ |
| `SET` 覆盖旧值 | 清除该 key 过期（TTL → -1） | ✅ |
| `EXPIRE` 覆盖旧过期 | 取新值 | ✅ |

### 原生 socket 自测（2026-11-18，WSL2 / gcc 13.3）

```
RESULT: 14/14 passed
  PASS  DEL 存在的 key -> 1，随后 GET nil
  PASS  DEL 不存在的 key -> 0
  PASS  DEL 多 key -> 删除个数（含混搭不存在）
  PASS  EXPIRE 不存在的 key -> 0
  PASS  EXPIRE 非数字参数 -> ERR not an integer
  PASS  SET/EXPIRE/TTL 全链：TTL 落在 [N-1, N]
  PASS  TTL 无过期 -> -1
  PASS  TTL 不存在 key -> -2
  PASS  惰性过期：EXPIRE 1s 后 GET -> nil（key 被回收）
  PASS  惰性过期：EXPIRE 后 TTL -> -2（TTL 触发回收）
  PASS  SET 覆盖清掉过期：SET 后再 TTL -> -1
  PASS  EXPIRE k 0 -> 立即删除
  PASS  EXPIRE 覆盖旧过期（5 -> 50）
  PASS  过期 key 被 GET 回收后，可正常重新 SET
```

### 真机互操作（redis-cli 7.0.15）

```
== 4. SET + EXPIRE 10s + TTL（应在 9~10 且单调递减）==
(integer) 1
(integer) 10
(integer) 9          <- TTL 递减证据
== 6. 惰性过期：EXPIRE 1s，sleep 后 GET -> (nil)、TTL -> -2 ==
(integer) 1
(nil)                <- 过期回收
(integer) -2
== 7. SET 覆盖清 TTL：SET -> EXPIRE -> SET -> TTL -> -1 ==
(integer) -1
== 8. EXPIRE k 0 -> 立即删除 ==
(integer) 1
(nil)
```

### 过程中的真 bug（M2 版面试素材）

首版 `EXPIRE` 判断 key 存在性时用 `dict_get(&g_db, key, NULL)` 做存在性探测，
而 `dict_get` 原实现命中后**无条件写 out** → NULL 解引用，服务器在第一个
`EXPIRE <存在的key>` 处段错误，测试从第 6 个用例起全部 Connection refused。
教训：**C 里"我传了 NULL 表示不关心"必须与接收方约定一致**；修复=dict_get 对
`out==NULL` 防御（这也是真 Redis `dictFetchValue` 让调用方每次传地址的原因）。
如果只修调用方不修接收方，同一个坑会在 EXPIRE/TTL/任何存在性探测处反复炸。

## 知识地图（覆盖边界）

- 写过级（本仓库覆盖）：IO 多路复用、RESP、哈希表/跳表、AOF 追加回放、惰性过期
- 读过级（面试必备，官方文档 + 源码注释）：穿透/击穿/雪崩、分布式锁与 Redlock、
  RDB vs AOF vs 混合、主从/哨兵/Cluster、SDS vs C 字符串、bigkey/热 key