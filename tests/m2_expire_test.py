#!/usr/bin/env python3
"""M2 验收：DEL / EXPIRE / TTL + 惰性过期（原生 socket 前置自测，风格同 m1_resp_test.py）。"""
import argparse
import socket
import subprocess
import sys
import threading
import time

PORT = 7783
CASES = []


def enc_cmd(*args):
    out = bytearray(b"*%d\r\n" % len(args))
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        out += b"$%d\r\n" % len(a) + a + b"\r\n"
    return bytes(out)


def recv_reply(sock):
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


def cmd(sock, *args):
    sock.sendall(enc_cmd(*args))
    return recv_reply(sock)


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


@case("DEL 存在的 key -> 1，随后 GET nil")
def t_del_existing():
    s = connect()
    assert cmd(s, "SET", "d1", "v")[0] == "simple"
    assert cmd(s, "DEL", "d1") == ("int", 1)
    assert cmd(s, "GET", "d1") == ("nil", None)
    s.close()


@case("DEL 不存在的 key -> 0")
def t_del_missing():
    s = connect()
    assert cmd(s, "DEL", "no-such-key") == ("int", 0)
    s.close()


@case("DEL 多 key -> 删除个数（含混搭不存在）")
def t_del_multi():
    s = connect()
    cmd(s, "SET", "m1", "a")
    cmd(s, "SET", "m2", "b")
    assert cmd(s, "DEL", "m1", "ghost", "m2", "m1") == ("int", 2)
    s.close()


@case("EXPIRE 不存在的 key -> 0")
def t_expire_missing():
    s = connect()
    assert cmd(s, "EXPIRE", "no-key", "100") == ("int", 0)
    s.close()


@case("EXPIRE 非数字参数 -> ERR not an integer")
def t_expire_badarg():
    s = connect()
    cmd(s, "SET", "e0", "v")
    r = cmd(s, "EXPIRE", "e0", "abc")
    assert r[0] == "err" and b"not an integer" in r[1], r
    s.close()


@case("SET/EXPIRE/TTL 全链：TTL 落在 [N-1, N]")
def t_ttl_ok():
    s = connect()
    cmd(s, "SET", "t1", "v")
    assert cmd(s, "EXPIRE", "t1", "100") == ("int", 1)
    ttl = cmd(s, "TTL", "t1")
    assert ttl[0] == "int" and 99 <= ttl[1] <= 100, ttl
    s.close()


@case("TTL 无过期 -> -1")
def t_ttl_none():
    s = connect()
    cmd(s, "SET", "t2", "v")
    assert cmd(s, "TTL", "t2") == ("int", -1)
    s.close()


@case("TTL 不存在 key -> -2")
def t_ttl_missing():
    s = connect()
    assert cmd(s, "TTL", "no-key") == ("int", -2)
    s.close()


@case("惰性过期：EXPIRE 1s 后 GET -> nil（key 被回收）")
def t_lazy_get():
    s = connect()
    cmd(s, "SET", "lx", "v")
    assert cmd(s, "EXPIRE", "lx", "1") == ("int", 1)
    time.sleep(1.2)
    assert cmd(s, "GET", "lx") == ("nil", None)
    s.close()


@case("惰性过期：EXPIRE 后 TTL -> -2（TTL 触发回收）")
def t_lazy_ttl():
    s = connect()
    cmd(s, "SET", "ly", "v")
    cmd(s, "EXPIRE", "ly", "1")
    time.sleep(1.2)
    assert cmd(s, "TTL", "ly") == ("int", -2)
    s.close()


@case("SET 覆盖清掉过期：SET 后再 TTL -> -1")
def t_set_clears_ttl():
    s = connect()
    cmd(s, "SET", "tc", "v")
    cmd(s, "EXPIRE", "tc", "100")
    cmd(s, "SET", "tc", "v2")
    assert cmd(s, "TTL", "tc") == ("int", -1)
    assert cmd(s, "GET", "tc") == ("bulk", b"v2")
    s.close()


@case("EXPIRE k 0 -> 立即删除")
def t_expire_zero():
    s = connect()
    cmd(s, "SET", "z", "v")
    assert cmd(s, "EXPIRE", "z", "0") == ("int", 1)
    assert cmd(s, "GET", "z") == ("nil", None)
    s.close()


@case("EXPIRE 覆盖旧过期（5 -> 50）")
def t_expire_overwrite():
    s = connect()
    cmd(s, "SET", "ow", "v")
    cmd(s, "EXPIRE", "ow", "5")
    assert cmd(s, "EXPIRE", "ow", "50") == ("int", 1)
    ttl = cmd(s, "TTL", "ow")
    assert ttl[0] == "int" and 49 <= ttl[1] <= 50, ttl
    s.close()


@case("过期 key 被 GET 回收后，可正常重新 SET")
def t_reset_after_expire():
    s = connect()
    cmd(s, "SET", "rr", "old")
    cmd(s, "EXPIRE", "rr", "1")
    time.sleep(1.2)
    assert cmd(s, "GET", "rr") == ("nil", None)
    assert cmd(s, "SET", "rr", "new") == ("simple", b"OK")
    assert cmd(s, "GET", "rr") == ("bulk", b"new")
    s.close()


def main():
    global PORT
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./mini-redis")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--final-sleep", type=float, default=0)
    args = ap.parse_args()

    PORT = args.port

    proc = subprocess.Popen([args.bin, str(PORT)], stderr=subprocess.DEVNULL)
    time.sleep(0.2)

    failed = 0
    for name, fn in CASES:
        try:
            fn()
            print("  PASS  %s" % name)
        except Exception as e:
            failed += 1
            print("  FAIL  %s: %r" % (name, e))

    proc.send_signal(subprocess.signal.SIGTERM)
    proc.wait(timeout=5)
    if args.final_sleep:
        time.sleep(args.final_sleep)

    print("RESULT: %d/%d passed" % (len(CASES) - failed, len(CASES)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()