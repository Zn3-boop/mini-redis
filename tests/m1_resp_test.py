#!/usr/bin/env python3
"""M1 验收（redis-cli 前置自测版）：原生 socket 按 RESP2 手工编解码。
证明协议层正确后，再让真机 redis-cli 互操作（独立步骤）。
"""
import argparse
import socket
import subprocess
import sys
import threading
import time

PORT = 7781
CASES = []


def enc_cmd(*args):
    """把参数编码成一条 RESP2 数组命令。"""
    out = bytearray(b"*%d\r\n" % len(args))
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        out += b"$%d\r\n" % len(a) + a + b"\r\n"
    return bytes(out)


def recv_reply(sock):
    """读一条 RESP2 回复，返回 (type, payload)。"""
    def read_line():
        buf = b""
        while not buf.endswith(b"\r\n"):
            ch = sock.recv(1)
            if not ch:
                raise ConnectionError("peer closed")
            buf += ch
        return buf[:-2]

    line = read_line()
    pre, body = line[:1], line[1:]
    if pre == b"+":
        return ("simple", body)
    if pre == b"-":
        return ("err", body)
    if pre == b":":
        return ("int", int(body))
    if pre == b"$":
        n = int(body)
        if n < 0:
            return ("nil", None)
        data = b""
        while len(data) < n + 2:
            chunk = sock.recv(n + 2 - len(data))
            if not chunk:
                raise ConnectionError("peer closed")
            data += chunk
        return ("bulk", data[:-2])
    raise ValueError("bad reply prefix: %r" % pre)


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=5)


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


@case("PING -> +PONG")
def t_ping():
    s = connect()
    s.sendall(enc_cmd("PING"))
    assert recv_reply(s) == ("simple", b"PONG")
    s.close()


@case("PING arg -> bulk")
def t_ping_arg():
    s = connect()
    s.sendall(enc_cmd("PING", "hello"))
    assert recv_reply(s) == ("bulk", b"hello")
    s.close()


@case("SET/GET roundtrip")
def t_set_get():
    s = connect()
    s.sendall(enc_cmd("SET", "foo", "bar"))
    assert recv_reply(s) == ("simple", b"OK")
    s.sendall(enc_cmd("GET", "foo"))
    assert recv_reply(s) == ("bulk", b"bar")
    s.close()


@case("GET missing -> nil")
def t_get_missing():
    s = connect()
    s.sendall(enc_cmd("GET", "nope"))
    assert recv_reply(s) == ("nil", None)
    s.close()


@case("binary-safe value (\\x00\\xff)")
def t_binary():
    s = connect()
    val = b"\x00\x01\xff\xfe"
    s.sendall(enc_cmd("SET", "bin", val))
    assert recv_reply(s) == ("simple", b"OK")
    s.sendall(enc_cmd("GET", "bin"))
    kind, payload = recv_reply(s)
    assert kind == "bulk" and payload == val, (kind, payload)
    s.close()


@case("case-insensitive cmd, case-sensitive key")
def t_case():
    s = connect()
    # 命令名大小写不敏感：set/GeT 都识别
    s.sendall(enc_cmd("set", "K", "v"))
    assert recv_reply(s) == ("simple", b"OK")
    s.sendall(enc_cmd("GeT", "K"))
    assert recv_reply(s) == ("bulk", b"v")
    # key 大小写敏感：K != k，get k 应为 nil（与真 Redis 同语义）
    s.sendall(enc_cmd("get", "k"))
    assert recv_reply(s) == ("nil", None)
    s.close()


@case("HELLO -> ERR (redis-cli 6+ 回落路径)")
def t_hello():
    s = connect()
    s.sendall(enc_cmd("HELLO", "3"))
    kind, payload = recv_reply(s)
    assert kind == "err" and b"unknown command" in payload, (kind, payload)
    s.close()


@case("wrong arg count -> ERR")
def t_wrong_args():
    s = connect()
    s.sendall(enc_cmd("SET", "onlykey"))
    kind, payload = recv_reply(s)
    assert kind == "err" and b"wrong number of arguments" in payload
    s.close()


@case("pipeline 3-in-1 packet")
def t_pipeline():
    s = connect()
    s.sendall(enc_cmd("SET", "p", "1") + enc_cmd("GET", "p") + enc_cmd("PING"))
    assert recv_reply(s) == ("simple", b"OK")
    assert recv_reply(s) == ("bulk", b"1")
    assert recv_reply(s) == ("simple", b"PONG")
    s.close()


@case("half packet (增量解析 NEED_MORE)")
def t_half_packet():
    s = connect()
    full = enc_cmd("SET", "half", "packet")
    s.sendall(full[:10])
    time.sleep(0.05)
    s.sendall(full[10:])
    assert recv_reply(s) == ("simple", b"OK")
    s.close()


@case("30 concurrent x 20 ops")
def t_concurrent():
    failures = []

    def worker(wid):
        try:
            s = connect()
            for i in range(20):
                k = "k%d_%d" % (wid, i)
                s.sendall(enc_cmd("SET", k, "v%d" % i))
                if recv_reply(s) != ("simple", b"OK"):
                    failures.append("SET %s" % k)
                s.sendall(enc_cmd("GET", k))
                if recv_reply(s) != ("bulk", ("v%d" % i).encode()):
                    failures.append("GET %s" % k)
            s.close()
        except Exception as e:  # noqa: BLE001
            failures.append(str(e))

    ts = [threading.Thread(target=worker, args=(i,)) for i in range(30)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not failures, failures[:5]


def wait_port(port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./mini-redis")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    proc = subprocess.Popen(
        [args.bin, str(args.port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        if not wait_port(args.port):
            out, _ = proc.communicate()
            print("server failed to start:\n", out)
            return 1
        failed = 0
        for name, fn in CASES:
            try:
                fn()
                print(f"  PASS  {name}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"  FAIL  {name}: {type(e).__name__}: {e}")
        print(f"RESULT: {len(CASES)-failed}/{len(CASES)} passed")
        return 0 if failed == 0 else 1
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        for line in (out or "").strip().splitlines():
            if "mini-redis" in line or "stats:" in line:
                print("[server]", line)


if __name__ == "__main__":
    sys.exit(main())