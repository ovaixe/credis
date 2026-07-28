#!/usr/bin/env python3
"""End-to-end checks against a running credis-server.

These drive the real wire protocol over a real socket, so they cover everything
the unit tests cannot: the event loop, partial reads and writes, pipelining,
connection lifecycle, and the exact bytes each command replies with.

Usage: integration.py [port]
"""

import socket
import sys
import time

from resp_client import Client, Error

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 6379

_passed = 0
_failed = 0
_section = ""


def section(name):
    global _section
    _section = name
    print("\n\033[1m%s\033[0m" % name)


def check(label, got, want):
    global _passed, _failed
    if got == want:
        _passed += 1
    else:
        _failed += 1
        print("  \033[31m✗\033[0m %s\n      got:  %r\n      want: %r" % (label, got, want))


def check_err(label, got, prefix):
    global _passed, _failed
    if isinstance(got, Error) and got.startswith(prefix):
        _passed += 1
    else:
        _failed += 1
        print("  \033[31m✗\033[0m %s\n      got:  %r\n      want error starting: %r"
              % (label, got, prefix))


def raw_exchange(payload, settle=1.0):
    """Sends raw bytes on a fresh connection and returns everything sent back."""
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    sock.sendall(payload)
    sock.settimeout(settle)
    data = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    sock.close()
    return data.decode(errors="replace")


def main():
    r = Client(port=PORT)
    r.cmd("FLUSHALL")

    # ------------------------------------------------------------------
    section("connection and server")
    check("PING", r.cmd("PING"), "PONG")
    check("PING with message", r.cmd("PING", "hello"), "hello")
    check("ECHO", r.cmd("ECHO", "hi"), "hi")
    check("SELECT", r.cmd("SELECT", "1"), "OK")
    check_err("SELECT out of range", r.cmd("SELECT", "99"), "ERR DB index is out of range")
    r.cmd("SELECT", "0")
    check("DBSIZE empty", r.cmd("DBSIZE"), 0)
    check("COMMAND COUNT > 100", r.cmd("COMMAND", "COUNT") > 100, True)
    check("CLIENT ID is positive", r.cmd("CLIENT", "ID") > 0, True)
    check("CLIENT SETNAME", r.cmd("CLIENT", "SETNAME", "tester"), "OK")
    check("CLIENT GETNAME", r.cmd("CLIENT", "GETNAME"), "tester")
    check("INFO has section", "# Server" in r.cmd("INFO"), True)
    check("CONFIG GET maxmemory", r.cmd("CONFIG", "GET", "maxmemory"), ["maxmemory", "0"])
    check("TIME returns two parts", len(r.cmd("TIME")), 2)
    check_err("unknown command", r.cmd("NOPE", "a"), "ERR unknown command 'NOPE'")
    check_err("wrong arity", r.cmd("GET"), "ERR wrong number of arguments for 'get' command")

    # ------------------------------------------------------------------
    section("strings")
    check("SET/GET", (r.cmd("SET", "k", "v"), r.cmd("GET", "k"))[1], "v")
    check("GET missing", r.cmd("GET", "absent"), None)
    check("binary safe", (r.cmd("SET", "b", b"a\x00b\xff"), len(r.cmd("GET", "b")))[1], 4)
    check("EXISTS counts duplicates", r.cmd("EXISTS", "k", "k", "absent"), 2)
    check("TYPE", r.cmd("TYPE", "k"), "string")
    check("TYPE missing", r.cmd("TYPE", "absent"), "none")
    check("APPEND", r.cmd("APPEND", "k", "2"), 2)
    check("STRLEN", r.cmd("STRLEN", "k"), 2)
    check("SETRANGE zero-pads", (r.cmd("DEL", "pad"), r.cmd("SETRANGE", "pad", "3", "x"),
                                 r.cmd("GET", "pad"))[2], "\x00\x00\x00x")
    check("GETRANGE negative", (r.cmd("SET", "s", "Hello World"),
                                r.cmd("GETRANGE", "s", "-5", "-1"))[1], "World")
    check("MSET/MGET", (r.cmd("MSET", "a", "1", "b", "2"),
                        r.cmd("MGET", "a", "b", "absent"))[1], ["1", "2", None])
    check("MSETNX is all-or-nothing", r.cmd("MSETNX", "a", "9", "fresh", "9"), 0)
    check("MSETNX wrote nothing", r.cmd("EXISTS", "fresh"), 0)
    check("INCR creates", (r.cmd("DEL", "n"), r.cmd("INCR", "n"))[1], 1)
    check("INCRBY", r.cmd("INCRBY", "n", "100"), 101)
    check("DECR", r.cmd("DECR", "n"), 100)
    check_err("INCR on non-integer", r.cmd("INCR", "k"),
              "ERR value is not an integer or out of range")
    check_err("INCR overflow", (r.cmd("SET", "big", "9223372036854775807"),
                                r.cmd("INCR", "big"))[1],
              "ERR increment or decrement would overflow")
    # Accumulated in long double, so this is 10.6 rather than 10.59999999999999964.
    check("INCRBYFLOAT precision", (r.cmd("SET", "f", "10.5"),
                                    r.cmd("INCRBYFLOAT", "f", "0.1"))[1], "10.6")
    check("GETDEL", (r.cmd("SET", "g", "v"), r.cmd("GETDEL", "g"), r.cmd("EXISTS", "g")),
          ("OK", "v", 0))

    section("SET options")
    r.cmd("DEL", "o")
    check("SET NX on missing", r.cmd("SET", "o", "1", "NX"), "OK")
    check("SET NX on existing", r.cmd("SET", "o", "2", "NX"), None)
    check("SET XX on existing", r.cmd("SET", "o", "3", "XX"), "OK")
    check("SET GET returns old", r.cmd("SET", "o", "4", "GET"), "3")
    check("SET EX sets TTL", (r.cmd("SET", "o", "5", "EX", "100"), r.cmd("TTL", "o"))[1], 100)
    check("plain SET clears TTL", (r.cmd("SET", "o", "6"), r.cmd("TTL", "o"))[1], -1)
    check("SET KEEPTTL", (r.cmd("SET", "o", "7", "EX", "50"),
                          r.cmd("SET", "o", "8", "KEEPTTL"), r.cmd("TTL", "o"))[2], 50)
    check_err("SET EX 0", r.cmd("SET", "o", "1", "EX", "0"),
              "ERR invalid expire time in 'set' command")

    # ------------------------------------------------------------------
    section("expiration")
    check("TTL of missing key", r.cmd("TTL", "ghost"), -2)
    check("TTL without expiry", (r.cmd("SET", "p", "v"), r.cmd("TTL", "p"))[1], -1)
    check("EXPIRE", r.cmd("EXPIRE", "p", "100"), 1)
    check("PERSIST", r.cmd("PERSIST", "p"), 1)
    check("PERSIST again", r.cmd("PERSIST", "p"), 0)
    check("EXPIRE on missing key", r.cmd("EXPIRE", "ghost", "10"), 0)

    r.cmd("SET", "g", "v")
    r.cmd("EXPIRE", "g", "100")
    check("EXPIRE GT rejects lower", r.cmd("EXPIRE", "g", "50", "GT"), 0)
    check("EXPIRE GT accepts higher", r.cmd("EXPIRE", "g", "200", "GT"), 1)
    check("EXPIRE LT accepts lower", r.cmd("EXPIRE", "g", "100", "LT"), 1)
    check("EXPIRE NX with TTL set", r.cmd("EXPIRE", "g", "300", "NX"), 0)
    r.cmd("PERSIST", "g")
    # A key with no TTL is treated as expiring at infinity.
    check("EXPIRE GT on persistent key", r.cmd("EXPIRE", "g", "300", "GT"), 0)
    check("EXPIRE LT on persistent key", r.cmd("EXPIRE", "g", "300", "LT"), 1)
    check_err("EXPIRE NX+XX", r.cmd("EXPIRE", "g", "10", "NX", "XX"), "ERR NX and XX")

    # Lazy expiry: the key must be invisible the moment it expires.
    r.cmd("SET", "gone", "v", "PX", "80")
    time.sleep(0.25)
    check("expired GET", r.cmd("GET", "gone"), None)
    check("expired EXISTS", r.cmd("EXISTS", "gone"), 0)
    check("expired TTL", r.cmd("TTL", "gone"), -2)

    # Active expiry: keys nobody touches are still reclaimed, by serverCron.
    r.cmd("FLUSHALL")
    payload = b"".join(Client.encode("SET", "e:%d" % i, "v", "PX", "120") for i in range(2000))
    r.send_raw(payload)
    for _ in range(2000):
        r.read_reply()
    check("2000 volatile keys stored", r.cmd("DBSIZE"), 2000)
    deadline = time.time() + 10
    while time.time() < deadline and r.cmd("DBSIZE") != 0:
        time.sleep(0.15)
    check("active expiry reclaimed them", r.cmd("DBSIZE"), 0)

    # Logically-expired keys must not leak through any enumeration path.
    r.cmd("DEBUG", "SET-ACTIVE-EXPIRE", "0")
    r.cmd("SET", "perm", "v")
    r.cmd("SET", "temp", "v", "PX", "60")
    time.sleep(0.2)
    check("KEYS hides expired", sorted(r.cmd("KEYS", "*")), ["perm"])
    check("SCAN hides expired", sorted(r.cmd("SCAN", "0", "COUNT", "1000")[1]), ["perm"])
    check("RANDOMKEY skips expired", r.cmd("RANDOMKEY"), "perm")
    r.cmd("DEBUG", "SET-ACTIVE-EXPIRE", "1")

    # ------------------------------------------------------------------
    section("keyspace")
    r.cmd("FLUSHALL")
    for key in ["user:1", "user:2", "admin:1", "x"]:
        r.cmd("SET", key, "v")
    check("KEYS *", sorted(r.cmd("KEYS", "*")), ["admin:1", "user:1", "user:2", "x"])
    check("KEYS prefix", sorted(r.cmd("KEYS", "user:*")), ["user:1", "user:2"])
    check("KEYS single char", r.cmd("KEYS", "?"), ["x"])
    check("KEYS char class", sorted(r.cmd("KEYS", "[ax]*")), ["admin:1", "x"])
    check("RENAME", (r.cmd("RENAME", "x", "y"), r.cmd("GET", "y"), r.cmd("EXISTS", "x")),
          ("OK", "v", 0))
    check_err("RENAME missing", r.cmd("RENAME", "nope", "z"), "ERR no such key")
    check("RENAME carries the TTL", (r.cmd("SET", "t", "v"), r.cmd("EXPIRE", "t", "500"),
                                     r.cmd("RENAME", "t", "t2"), r.cmd("TTL", "t2"))[3], 500)
    check("COPY", r.cmd("COPY", "y", "ycopy"), 1)
    check("COPY refuses to clobber", r.cmd("COPY", "y", "ycopy"), 0)
    check("COPY REPLACE", r.cmd("COPY", "y", "ycopy", "REPLACE"), 1)
    check("OBJECT ENCODING int", (r.cmd("SET", "num", "123"),
                                  r.cmd("OBJECT", "ENCODING", "num"))[1], "int")
    check("SWAPDB", r.cmd("SWAPDB", "0", "1"), "OK")
    r.cmd("SWAPDB", "0", "1")

    # SCAN must reach every key that was present for the whole iteration.
    r.cmd("FLUSHALL")
    expected = set("k:%d" % i for i in range(5000))
    r.send_raw(b"".join(Client.encode("SET", k, "v") for k in expected))
    for _ in expected:
        r.read_reply()
    seen, cursor, rounds = set(), "0", 0
    while True:
        cursor, batch = r.cmd("SCAN", cursor, "COUNT", "100")
        seen.update(batch)
        rounds += 1
        if cursor == "0" or rounds > 100000:
            break
    check("SCAN reached every key", seen, expected)
    check("SCAN took several rounds", rounds > 1, True)
    check("SCAN MATCH", sorted(r.cmd("SCAN", "0", "MATCH", "k:1?", "COUNT", "10000")[1]),
          sorted("k:1%d" % i for i in range(10)))
    check("SCAN TYPE filter", r.cmd("SCAN", "0", "TYPE", "list", "COUNT", "10000")[1], [])
    check_err("SCAN bad cursor", r.cmd("SCAN", "abc"), "ERR invalid cursor")

    # ------------------------------------------------------------------
    section("lists")
    r.cmd("FLUSHALL")
    check("RPUSH", r.cmd("RPUSH", "L", "a", "b", "c"), 3)
    check("LPUSH", r.cmd("LPUSH", "L", "z"), 4)
    check("LRANGE all", r.cmd("LRANGE", "L", "0", "-1"), ["z", "a", "b", "c"])
    check("LRANGE negative", r.cmd("LRANGE", "L", "-2", "-1"), ["b", "c"])
    check("LINDEX -1", r.cmd("LINDEX", "L", "-1"), "c")
    check("LSET", (r.cmd("LSET", "L", "0", "Z"), r.cmd("LINDEX", "L", "0"))[1], "Z")
    check_err("LSET out of range", r.cmd("LSET", "L", "99", "x"), "ERR index out of range")
    check("LPOP with count", (r.cmd("LPOP", "L"), r.cmd("LPOP", "L", "2"))[1], ["a", "b"])
    check("emptied list is removed", (r.cmd("RPOP", "L"), r.cmd("EXISTS", "L"))[1], 0)
    check("LPOP on missing key", r.cmd("LPOP", "absent"), None)
    r.cmd("RPUSH", "L3", "a", "b", "a", "c", "a")
    check("LREM from head", r.cmd("LREM", "L3", "2", "a"), 2)
    check("LREM result", r.cmd("LRANGE", "L3", "0", "-1"), ["b", "c", "a"])
    r.cmd("DEL", "L4")
    r.cmd("RPUSH", "L4", "a", "b", "c", "d", "e")
    check("LTRIM", (r.cmd("LTRIM", "L4", "1", "3"), r.cmd("LRANGE", "L4", "0", "-1"))[1],
          ["b", "c", "d"])
    r.cmd("DEL", "L5")
    r.cmd("RPUSH", "L5", "a", "c")
    check("LINSERT BEFORE", (r.cmd("LINSERT", "L5", "BEFORE", "c", "b"),
                             r.cmd("LRANGE", "L5", "0", "-1"))[1], ["a", "b", "c"])
    check("LINSERT missing pivot", r.cmd("LINSERT", "L5", "BEFORE", "zz", "x"), -1)
    r.cmd("DEL", "LP")
    r.cmd("RPUSH", "LP", "a", "b", "c", "a", "b", "c", "a")
    check("LPOS", r.cmd("LPOS", "LP", "a"), 0)
    check("LPOS RANK 2", r.cmd("LPOS", "LP", "a", "RANK", "2"), 3)
    check("LPOS RANK -1", r.cmd("LPOS", "LP", "a", "RANK", "-1"), 6)
    check("LPOS COUNT 0", r.cmd("LPOS", "LP", "a", "COUNT", "0"), [0, 3, 6])
    r.cmd("DEL", "src", "dst")
    r.cmd("RPUSH", "src", "a", "b", "c")
    check("RPOPLPUSH", r.cmd("RPOPLPUSH", "src", "dst"), "c")
    check("LMOVE LEFT RIGHT", r.cmd("LMOVE", "src", "dst", "LEFT", "RIGHT"), "a")
    check("LMOVE result", r.cmd("LRANGE", "dst", "0", "-1"), ["c", "a"])
    r.cmd("DEL", "rot")
    r.cmd("RPUSH", "rot", "a")
    # Rotating a single-element list onto itself must not lose the element.
    check("RPOPLPUSH onto itself", r.cmd("RPOPLPUSH", "rot", "rot"), "a")
    check("self-rotate kept it", r.cmd("LRANGE", "rot", "0", "-1"), ["a"])
    check_err("wrong type", (r.cmd("SET", "str", "v"), r.cmd("LPUSH", "str", "x"))[1], "WRONGTYPE")

    # ------------------------------------------------------------------
    section("hashes")
    r.cmd("FLUSHALL")
    check("HSET", r.cmd("HSET", "H", "f1", "v1", "f2", "v2"), 2)
    check("HSET update returns 0", r.cmd("HSET", "H", "f1", "x"), 0)
    check("HGET", r.cmd("HGET", "H", "f1"), "x")
    check("HMGET", r.cmd("HMGET", "H", "f1", "f2", "zz"), ["x", "v2", None])
    check("HKEYS", sorted(r.cmd("HKEYS", "H")), ["f1", "f2"])
    hgetall = r.cmd("HGETALL", "H")
    check("HGETALL flat in RESP2", dict(zip(hgetall[::2], hgetall[1::2])),
          {"f1": "x", "f2": "v2"})
    check("HSETNX", r.cmd("HSETNX", "H", "f3", "v3"), 1)
    check("HSETNX existing", r.cmd("HSETNX", "H", "f3", "zz"), 0)
    check("HINCRBY", (r.cmd("HSET", "H", "n", "10"), r.cmd("HINCRBY", "H", "n", "5"))[1], 15)
    check_err("HINCRBY non-integer", r.cmd("HINCRBY", "H", "f1", "1"),
              "ERR hash value is not an integer")
    check("HINCRBYFLOAT", (r.cmd("HSET", "H", "fl", "10.5"),
                           r.cmd("HINCRBYFLOAT", "H", "fl", "0.1"))[1], "10.6")
    check("emptied hash is removed", (r.cmd("HSET", "HD", "a", "1"), r.cmd("HDEL", "HD", "a"),
                                      r.cmd("EXISTS", "HD"))[2], 0)

    args = ["HSET", "BIGH"]
    for i in range(1000):
        args += ["f:%d" % i, "v:%d" % i]
    r.cmd(*args)
    seen, cursor, rounds = {}, "0", 0
    while True:
        cursor, batch = r.cmd("HSCAN", "BIGH", cursor, "COUNT", "50")
        for i in range(0, len(batch), 2):
            seen[batch[i]] = batch[i + 1]
        rounds += 1
        if cursor == "0" or rounds > 100000:
            break
    check("HSCAN reached every field", len(seen), 1000)
    check("HSCAN values intact", seen["f:500"], "v:500")
    check("HSCAN NOVALUES", len(r.cmd("HSCAN", "BIGH", "0", "COUNT", "10000", "NOVALUES")[1]),
          1000)

    # ------------------------------------------------------------------
    section("sets")
    r.cmd("FLUSHALL")
    check("SADD", r.cmd("SADD", "S", "a", "b", "c"), 3)
    check("SADD duplicate", r.cmd("SADD", "S", "a"), 0)
    check("SMEMBERS", sorted(r.cmd("SMEMBERS", "S")), ["a", "b", "c"])
    check("SMISMEMBER", r.cmd("SMISMEMBER", "S", "a", "zz", "c"), [1, 0, 1])
    check("SREM", r.cmd("SREM", "S", "a", "zz"), 1)
    r.cmd("SADD", "s1", "a", "b", "c", "d")
    r.cmd("SADD", "s2", "b", "c")
    r.cmd("SADD", "s3", "c", "d", "e")
    check("SINTER", sorted(r.cmd("SINTER", "s1", "s2")), ["b", "c"])
    check("SINTER of three", sorted(r.cmd("SINTER", "s1", "s2", "s3")), ["c"])
    check("SUNION", sorted(r.cmd("SUNION", "s2", "s3")), ["b", "c", "d", "e"])
    check("SDIFF", sorted(r.cmd("SDIFF", "s1", "s2")), ["a", "d"])
    check("SINTERSTORE", r.cmd("SINTERSTORE", "dst", "s1", "s2"), 2)
    check("empty STORE result deletes dest", (r.cmd("SDIFFSTORE", "d3", "s1", "s1"),
                                              r.cmd("EXISTS", "d3"))[1], 0)
    check("SINTERCARD", r.cmd("SINTERCARD", "2", "s1", "s2"), 2)
    check("SINTERCARD LIMIT", r.cmd("SINTERCARD", "2", "s1", "s2", "LIMIT", "1"), 1)
    check("SMOVE", (r.cmd("SMOVE", "s1", "s2", "a"), r.cmd("SISMEMBER", "s1", "a"),
                    r.cmd("SISMEMBER", "s2", "a")), (1, 0, 1))
    r.cmd("SADD", "BIGS", *["m:%d" % i for i in range(1000)])
    seen, cursor, rounds = set(), "0", 0
    while True:
        cursor, batch = r.cmd("SSCAN", "BIGS", cursor, "COUNT", "50")
        seen.update(batch)
        rounds += 1
        if cursor == "0" or rounds > 100000:
            break
    check("SSCAN reached every member", len(seen), 1000)

    # ------------------------------------------------------------------
    section("sorted sets")
    r.cmd("FLUSHALL")
    check("ZADD", r.cmd("ZADD", "Z", "1", "a", "2", "b", "3", "c"), 3)
    check("ZADD update returns 0", r.cmd("ZADD", "Z", "5", "a"), 0)
    check("ZADD CH counts updates", r.cmd("ZADD", "Z", "CH", "6", "a"), 1)
    check("ZSCORE", r.cmd("ZSCORE", "Z", "a"), "6")
    check("ZMSCORE", r.cmd("ZMSCORE", "Z", "a", "zz", "b"), ["6", None, "2"])
    check("ZRANK", r.cmd("ZRANK", "Z", "b"), 0)
    check("ZREVRANK", r.cmd("ZREVRANK", "Z", "a"), 0)
    check("ZRANK WITHSCORE", r.cmd("ZRANK", "Z", "b", "WITHSCORE"), [0, "2"])
    check_err("ZADD flags must precede pairs", r.cmd("ZADD", "Z", "1", "m", "CH"),
              "ERR wrong number of arguments for 'zadd' command")

    r.cmd("DEL", "Z")
    r.cmd("ZADD", "Z", "1", "a", "2", "b", "3", "c", "4", "d")
    check("ZRANGE", r.cmd("ZRANGE", "Z", "0", "-1"), ["a", "b", "c", "d"])
    check("ZRANGE WITHSCORES", r.cmd("ZRANGE", "Z", "0", "1", "WITHSCORES"),
          ["a", "1", "b", "2"])
    check("ZRANGE REV", r.cmd("ZRANGE", "Z", "0", "-1", "REV"), ["d", "c", "b", "a"])
    check("ZRANGEBYSCORE", r.cmd("ZRANGEBYSCORE", "Z", "2", "3"), ["b", "c"])
    check("ZRANGEBYSCORE exclusive", r.cmd("ZRANGEBYSCORE", "Z", "(2", "(4"), ["c"])
    check("ZRANGEBYSCORE infinities", r.cmd("ZRANGEBYSCORE", "Z", "-inf", "+inf"),
          ["a", "b", "c", "d"])
    check("ZRANGEBYSCORE LIMIT", r.cmd("ZRANGEBYSCORE", "Z", "-inf", "+inf", "LIMIT", "1", "2"),
          ["b", "c"])
    check("ZREVRANGEBYSCORE", r.cmd("ZREVRANGEBYSCORE", "Z", "3", "2"), ["c", "b"])
    check("ZCOUNT", r.cmd("ZCOUNT", "Z", "2", "3"), 2)
    check("ZCOUNT exclusive", r.cmd("ZCOUNT", "Z", "(1", "(4"), 2)
    check_err("ZRANGEBYSCORE bad bound", r.cmd("ZRANGEBYSCORE", "Z", "abc", "3"),
              "ERR min or max is not a float")

    r.cmd("DEL", "L")
    r.cmd("ZADD", "L", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e")
    check("ZRANGEBYLEX all", r.cmd("ZRANGEBYLEX", "L", "-", "+"), ["a", "b", "c", "d", "e"])
    check("ZRANGEBYLEX inclusive", r.cmd("ZRANGEBYLEX", "L", "[b", "[d"), ["b", "c", "d"])
    check("ZRANGEBYLEX exclusive", r.cmd("ZRANGEBYLEX", "L", "(b", "(d"), ["c"])
    check("ZREVRANGEBYLEX", r.cmd("ZREVRANGEBYLEX", "L", "[d", "[b"), ["d", "c", "b"])
    check("ZLEXCOUNT", r.cmd("ZLEXCOUNT", "L", "[b", "[d"), 3)
    check_err("ZRANGEBYLEX needs a marker", r.cmd("ZRANGEBYLEX", "L", "b", "d"),
              "ERR min or max not valid string range item")

    r.cmd("DEL", "G")
    r.cmd("ZADD", "G", "5", "m")
    check("ZADD GT rejects lower", (r.cmd("ZADD", "G", "GT", "3", "m"),
                                    r.cmd("ZSCORE", "G", "m"))[1], "5")
    check("ZADD GT accepts higher", (r.cmd("ZADD", "G", "GT", "9", "m"),
                                     r.cmd("ZSCORE", "G", "m"))[1], "9")
    check("ZADD LT accepts lower", (r.cmd("ZADD", "G", "LT", "2", "m"),
                                    r.cmd("ZSCORE", "G", "m"))[1], "2")
    check("ZADD INCR", (r.cmd("DEL", "I"), r.cmd("ZADD", "I", "1", "a"),
                        r.cmd("ZADD", "I", "INCR", "2", "a"))[2], "3")
    check("ZADD NX INCR on existing", r.cmd("ZADD", "I", "NX", "INCR", "2", "a"), None)
    check("ZINCRBY", r.cmd("ZINCRBY", "I", "5", "a"), "8")

    r.cmd("DEL", "R")
    r.cmd("ZADD", "R", "1", "a", "2", "b", "3", "c", "4", "d", "5", "e")
    check("ZREMRANGEBYRANK", r.cmd("ZREMRANGEBYRANK", "R", "0", "1"), 2)
    check("ZREMRANGEBYRANK result", r.cmd("ZRANGE", "R", "0", "-1"), ["c", "d", "e"])
    check("ZREMRANGEBYSCORE", r.cmd("ZREMRANGEBYSCORE", "R", "3", "4"), 2)
    check("emptied zset is removed", (r.cmd("ZREMRANGEBYSCORE", "R", "-inf", "+inf"),
                                      r.cmd("EXISTS", "R"))[1], 0)

    r.cmd("DEL", "P")
    r.cmd("ZADD", "P", "1", "a", "2", "b", "3", "c")
    check("ZPOPMIN", r.cmd("ZPOPMIN", "P"), ["a", "1"])
    check("ZPOPMAX", r.cmd("ZPOPMAX", "P"), ["c", "3"])

    r.cmd("DEL", "z1", "z2")
    r.cmd("ZADD", "z1", "1", "a", "2", "b")
    r.cmd("ZADD", "z2", "3", "b", "4", "c")
    check("ZUNIONSTORE", r.cmd("ZUNIONSTORE", "zu", "2", "z1", "z2"), 3)
    check("ZUNIONSTORE sums scores", r.cmd("ZRANGE", "zu", "0", "-1", "WITHSCORES"),
          ["a", "1", "c", "4", "b", "5"])
    check("ZINTER", r.cmd("ZINTER", "2", "z1", "z2"), ["b"])
    check("ZDIFF", r.cmd("ZDIFF", "2", "z1", "z2"), ["a"])
    check("WEIGHTS", (r.cmd("ZUNIONSTORE", "zw", "2", "z1", "z2", "WEIGHTS", "2", "3"),
                      r.cmd("ZSCORE", "zw", "b"))[1], "13")
    check("AGGREGATE MAX", (r.cmd("ZUNIONSTORE", "zm", "2", "z1", "z2", "AGGREGATE", "MAX"),
                            r.cmd("ZSCORE", "zm", "b"))[1], "3")
    check("ZRANGESTORE", r.cmd("ZRANGESTORE", "zr", "z1", "0", "-1"), 2)

    r.cmd("DEL", "F")
    r.cmd("ZADD", "F", "1.5", "x", "-0.25", "y", "3", "z", "+inf", "big", "-inf", "small")
    check("infinite scores order correctly", r.cmd("ZRANGE", "F", "0", "-1"),
          ["small", "y", "x", "z", "big"])
    check("inf formats as inf", r.cmd("ZSCORE", "F", "big"), "inf")

    # ------------------------------------------------------------------
    section("protocol")
    # A command split across many packets must parse identically.
    request = Client.encode("SET", "split", "value")
    for byte in request[:-1]:
        r.send_raw(bytes([byte]))
        time.sleep(0.0004)
    r.send_raw(request[-1:])
    check("byte-at-a-time command", r.read_reply(), "OK")
    check("byte-at-a-time value", r.cmd("GET", "split"), "value")

    # Split in the middle of a bulk payload.
    r.send_raw(b"*3\r\n$3\r\nSET\r\n$3\r\nch1\r\n$10\r\nabcde")
    time.sleep(0.05)
    r.send_raw(b"fghij\r\n")
    check("split mid-payload", r.read_reply(), "OK")
    check("split mid-payload value", r.cmd("GET", "ch1"), "abcdefghij")

    # Pipelining: one write, one read, many commands.
    count = 1000
    r.send_raw(b"".join(Client.encode("SET", "pk%d" % i, "v") for i in range(count)))
    replies = [r.read_reply() for _ in range(count)]
    check("1000 pipelined commands", replies.count("OK"), count)

    # A large reply forces partial writes and the EPOLLOUT path.
    big = "x" * (8 * 1024 * 1024)
    r.cmd("SET", "big", big)
    check("8MB round trip", r.cmd("GET", "big") == big, True)

    check("inline command over raw socket", raw_exchange(b"PING\r\n"), "+PONG\r\n")
    check("inline with quotes",
          raw_exchange(b'SET inline "a b"\r\nGET inline\r\n'), "+OK\r\n$3\r\na b\r\n")
    check("empty multibulk is skipped",
          raw_exchange(b"*0\r\n*1\r\n$4\r\nPING\r\n"), "+PONG\r\n")

    for payload, expected in [
        (b"*abc\r\n", "-ERR Protocol error: invalid multibulk length"),
        (b"*1\r\n+PING\r\n", "-ERR Protocol error: expected '$', got '+'"),
        (b"*1\r\n$abc\r\n", "-ERR Protocol error: invalid bulk length"),
        (b"*1\r\n$99999999999\r\n", "-ERR Protocol error: invalid bulk length"),
        (b'SET "unclosed\r\n', "-ERR Protocol error: unbalanced quotes in request"),
    ]:
        check("protocol error: %s" % expected[5:40],
              raw_exchange(payload).startswith(expected), True)

    # ------------------------------------------------------------------
    section("RESP3")
    r3 = Client(port=PORT)
    hello = r3.cmd("HELLO", "3")
    check("HELLO 3 replies with a map", isinstance(hello, dict), True)
    check("HELLO 3 proto", hello.get("proto"), 3)
    check("RESP3 null", r3.cmd("GET", "definitely-missing"), None)
    r3.cmd("DEL", "h3")
    r3.cmd("HSET", "h3", "a", "1", "b", "2")
    check("HGETALL is a map in RESP3", r3.cmd("HGETALL", "h3"), {"a": "1", "b": "2"})
    r3.cmd("DEL", "z3")
    r3.cmd("ZADD", "z3", "1", "a")
    check("ZSCORE is a double in RESP3", r3.cmd("ZSCORE", "z3", "a"), 1.0)
    check_err("HELLO 9", r3.cmd("HELLO", "9"), "NOPROTO unsupported protocol version")
    r3.close()

    # ------------------------------------------------------------------
    section("connections")
    clients = [Client(port=PORT) for _ in range(50)]
    for i, client in enumerate(clients):
        client.cmd("SET", "c:%d" % i, str(i))
    check("50 concurrent clients", all(clients[i].cmd("GET", "c:%d" % i) == str(i)
                                       for i in range(50)), True)
    check("INFO reports them", "connected_clients:" in clients[0].cmd("INFO", "clients"), True)
    for client in clients:
        client.close()

    # Each connection keeps its own selected database and protocol.
    a = Client(port=PORT)
    b = Client(port=PORT)
    a.cmd("SELECT", "3")
    a.cmd("SET", "isolated", "yes")
    check("per-connection database", b.cmd("GET", "isolated"), None)
    b.cmd("SELECT", "3")
    check("same database sees it", b.cmd("GET", "isolated"), "yes")
    a.cmd("FLUSHALL")
    a.close()
    b.close()

    r.cmd("FLUSHALL")
    r.close()

    print("\n%d passed, %d failed" % (_passed, _failed))
    return 0 if _failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
