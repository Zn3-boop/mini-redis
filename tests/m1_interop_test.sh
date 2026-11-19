#!/usr/bin/env bash
# M1 硬验收：真机 redis-cli 互操作（需要 redis-tools 已安装）
# 用法：bash tests/m1_interop_test.sh [port]
set -u
PORT="${1:-7782}"
BIN="./mini-redis"
LOG=$(mktemp)

echo "== redis-cli 版本 =="
redis-cli --version

"$BIN" "$PORT" >"$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

# 等端口就绪（最多 5s）
for _ in $(seq 1 50); do
    if redis-cli -p "$PORT" PING >/dev/null 2>&1; then break; fi
    sleep 0.1
done

echo "== 1. PING =="
redis-cli -p "$PORT" PING

echo "== 2. SET / GET =="
redis-cli -p "$PORT" SET hello world
redis-cli -p "$PORT" GET hello

echo "== 3. GET 不存在的 key -> (nil) =="
redis-cli -p "$PORT" GET missing-key

echo "== 4. HELLO 回落证据：redis-cli 6+ 自动握手（--verbose 显示发出的命令）=="
redis-cli --verbose -p "$PORT" PING 2>&1 | head -8

echo "== 5. 未实现命令 -> 干净报错不崩（MTGET 为例）=="
redis-cli -p "$PORT" MTGET a b 2>&1 || true

echo "== 6. --pipe 批量 RESP =="
printf 'SET k1 v1\r\nSET k2 v2\r\nGET k1\r\n' | redis-cli -p "$PORT" --pipe 2>&1
redis-cli -p "$PORT" GET k2

echo "== server 日志 =="
cat "$LOG"