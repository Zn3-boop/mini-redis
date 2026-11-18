#!/usr/bin/env python3
"""M0 验收：多客户端并发压测，验证 epoll echo server 不断链、不丢字节。

用例：
  A) CONCURRENT 个客户端 x ROUNDS 轮，每轮发送随机长度 payload，逐个核对回显；
  B) N 个大包客户端，一次性发送 64KB，核对逐字节回显（专门压 ET 排空循环）。

退出码：0 = 全部通过；1 = 任一失败。
"""
import argparse
import random
import socket
import subprocess
import sys
import time

CONCURRENT = 30   # 并发客户端数
ROUNDS     = 40   # 每客户端轮数
BIG_CLIENTS = 8   # 大包客户端数
BIG_SIZE    = 64 * 1024


def recv_exact(sock, n: int, timeout: float = 10.0) -> bytes:
    sock.settimeout(timeout)
    chunks = []
    got = 0
    while got < n:
        chunk = sock.recv(n - got)
        if not chunk:
            raise ConnectionError(f"peer closed early: need {n}, got {got}")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def wait_port(port: int, timeout: float = 5.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def test_small_rounds(port: int) -> list[str]:
    """每客户端独立连接，多轮小/中包回显核对。"""
    failures = []

    def client(cid: int):
        rng = random.Random(cid)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                for r in range(ROUNDS):
                    size = rng.choice([16, 256, 4096])
                    payload = rng.randbytes(size)
                    s.sendall(payload)
                    echo = recv_exact(s, size)
                    if echo != payload:
                        failures.append(f"client {cid} round {r}: MISMATCH (len {len(echo)})")
                        return
        except Exception as e:  # noqa: BLE001
            failures.append(f"client {cid}: {type(e).__name__}: {e}")

    import threading
    threads = [threading.Thread(target=client, args=(i,)) for i in range(CONCURRENT)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return failures


def test_big_packets(port: int) -> list[str]:
    """64KB 单包，一个包内压满内核缓冲区，验证 ET 读循环逐字节完整。"""
    failures = []
    for i in range(BIG_CLIENTS):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                payload = random.Random(i).randbytes(BIG_SIZE)
                s.sendall(payload)
                echo = recv_exact(s, BIG_SIZE)
                if echo != payload:
                    failures.append(f"big client {i}: MISMATCH (len {len(echo)})")
        except Exception as e:  # noqa: BLE001
            failures.append(f"big client {i}: {type(e).__name__}: {e}")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default="./echo-server")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--mode", choices=["et", "lt"], default="et")
    args = ap.parse_args()

    log = lambda *a: print(*a, flush=True)  # noqa: E731
    proc = subprocess.Popen(
        [args.bin, str(args.port), args.mode],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        if not wait_port(args.port):
            log("FAIL: server did not come up on port", args.port)
            return 1

        started = time.monotonic()
        failures = test_small_rounds(args.port)
        failures += test_big_packets(args.port)
        elapsed = time.monotonic() - started

        total = CONCURRENT * ROUNDS + BIG_CLIENTS
        ok = total - len(failures)
        log(f"mode={args.mode} concurrent={CONCURRENT} rounds={CONCURRENT*ROUNDS} "
            f"big_packets={BIG_CLIENTS}")
        log(f"PASS {ok}/{total}  FAIL {len(failures)}  elapsed={elapsed:.2f}s")
        for f in failures[:20]:
            log(f"  - {f}")
        return 0 if not failures else 1
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        for line in (out or "").strip().splitlines():
            if "echo-server" in line or "stats:" in line:
                log("[server]", line)


if __name__ == "__main__":
    sys.exit(main())