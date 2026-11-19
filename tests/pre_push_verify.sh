#!/usr/bin/env bash
# mini-redis 公开前最终验证 + 状态快照
set -u
cd /mnt/d/mini-redis || exit 1

echo "== 自测（原生 socket）=="
make clean >/dev/null 2>&1
make >/dev/null 2>&1
python3 tests/m1_resp_test.py --bin ./mini-redis 2>&1 | grep RESULT
python3 tests/m2_expire_test.py --bin ./mini-redis 2>&1 | grep RESULT

echo "== M1 互操作关键行 =="
bash tests/m1_interop_test.sh > /tmp/v1.txt 2>&1
grep -E "replies:|v2" /tmp/v1.txt

echo "== M2 互操作关键行 =="
bash tests/m2_interop_test.sh > /tmp/v2.txt 2>&1
grep -E "integer|nil|TTL" /tmp/v2.txt | head -14

echo "== git 状态 =="
git status --short
git --no-pager log --oneline -6
echo "-- LICENSE --"
ls -la LICENSE*