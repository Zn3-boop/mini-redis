#!/usr/bin/env bash
# M2 硬验收：真机 redis-cli 验证 DEL/EXPIRE/TTL + 惰性过期（需要 redis-tools）
# 用法：bash tests/m2_interop_test.sh [port]
set -u
PORT="${1:-7784}"
BIN="./mini-redis"
LOG=$(mktemp)

"$BIN" "$PORT" >"$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

for _ in $(seq 1 50); do
    if redis-cli -p "$PORT" PING >/dev/null 2>&1; then break; fi
    sleep 0.1
done

echo "== 1. DEL：删除 -> 1，再 GET -> (nil) =="
redis-cli -p "$PORT" SET k1 hello >/dev/null
redis-cli -p "$PORT" DEL k1
redis-cli -p "$PORT" GET k1

echo "== 2. DEL 不存在 -> (integer) 0 =="
redis-cli -p "$PORT" DEL missing

echo "== 3. EXPIRE 不存在 key -> (integer) 0 =="
redis-cli -p "$PORT" EXPIRE missing 10

echo "== 4. SET + EXPIRE 10s + TTL（应在 9~10 且单调递减）=="
redis-cli -p "$PORT" SET t1 hello >/dev/null
redis-cli -p "$PORT" EXPIRE t1 10
redis-cli -p "$PORT" TTL t1
sleep 1
redis-cli -p "$PORT" TTL t1

echo "== 5. TTL 无过期 -> -1，TTL 不存在 -> -2 =="
redis-cli -p "$PORT" TTL k1
redis-cli -p "$PORT" TTL no-such-key

echo "== 6. 惰性过期：EXPIRE 1s，sleep 后 GET -> (nil)、TTL -> -2 =="
redis-cli -p "$PORT" SET e1 hello >/dev/null
redis-cli -p "$PORT" EXPIRE e1 1
sleep 1.2
redis-cli -p "$PORT" GET e1
redis-cli -p "$PORT" TTL e1

echo "== 7. SET 覆盖清 TTL：SET -> EXPIRE -> SET -> TTL -> -1 =="
redis-cli -p "$PORT" SET s1 a >/dev/null
redis-cli -p "$PORT" EXPIRE s1 100 >/dev/null
redis-cli -p "$PORT" SET s1 b >/dev/null
redis-cli -p "$PORT" TTL s1

echo "== 8. EXPIRE k 0 -> 立即删除 =="
redis-cli -p "$PORT" SET z1 v >/dev/null
redis-cli -p "$PORT" EXPIRE z1 0
redis-cli -p "$PORT" GET z1

echo "== server 日志 =="
cat "$LOG"