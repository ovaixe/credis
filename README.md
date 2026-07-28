# credis

A Redis server written from scratch in C++23 — the protocol, the event loop, the
hash table, the skiplist and the expiry machinery are all implemented here rather
than delegated to a library. Any Redis client can connect to it.

```console
$ credis-server --port 6379
$ redis-cli -p 6379
127.0.0.1:6379> set greeting "hello"
OK
127.0.0.1:6379> zadd leaderboard 100 alice 250 bob
(integer) 2
127.0.0.1:6379> zrange leaderboard 0 -1 WITHSCORES
1) "alice"
2) "100"
3) "bob"
4) "250"
```

Only the standard library and POSIX are used. No hiredis, no libuv, no asio,
no Boost — and the tests use a hand-rolled harness rather than a framework.

## Building

```sh
cmake -B build
cmake --build build -j
```

Requires CMake 3.20+ and a C++23 compiler (developed against GCC 16).

Run everything:

```sh
cd build && ctest --output-on-failure
```

That runs the unit tests (`credis-tests`) and the end-to-end suite
(`tests/integration.sh`), which starts a real server on a scratch port and
drives it over a socket.

A sanitizer build, for the memory-unsafe parts (the dict and skiplist both use
raw pointers):

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=ASan && cmake --build build-asan -j
cd build-asan && ctest --output-on-failure
```

## Running

```sh
credis-server                      # 127.0.0.1:6379
credis-server --port 7000
credis-server credis.conf
credis-server credis.conf --port 7000   # file first, flags override
```

`--help` lists every option. An existing `redis.conf` will usually load: settings
credis does not implement (`appendonly`, `save`, `daemonize`, …) are accepted and
ignored rather than treated as errors.

Because the parser accepts inline commands, you can talk to it with nothing but
a socket:

```console
$ printf 'SET greeting "hello there"\r\nGET greeting\r\n' | nc 127.0.0.1 6379
+OK
$11
hello there
```

## Architecture

One thread. One `epoll` loop owns every socket. A command runs to completion
against the keyspace before the next one starts, so there are no locks anywhere
and no concurrency bugs to reason about — the same design decision real Redis
makes.

```
 epoll_wait ──┬─ listener readable ──► accept() ──► Connection (non-blocking)
              │
              ├─ client readable ────► read() into the query buffer
              │                        └─ parse as many complete commands as
              │                           arrived, execute each, append replies
              │                        └─ write out; if partial, arm EPOLLOUT
              │
              ├─ client writable ────► resume the write; disarm when drained
              │
              └─ every 100ms ────────► serverCron: the active expiry cycle
```

| Path | What lives there |
|---|---|
| [src/net/](src/net/) | `Buffer`, the epoll reactor, the listener, per-client connections |
| [src/resp/](src/resp/) | The incremental request parser and the reply writer |
| [src/store/](src/store/) | `Dict`, `SkipList`/`ZSet`, `Object`, and the database |
| [src/commands/](src/commands/) | The command table and every command implementation |
| [src/util/](src/util/) | Numeric conversion, glob matching, argument splitting, logging |

### The pieces worth reading

**[`Dict<V>`](src/store/dict.h)** is the reason this project does not use
`std::unordered_map`. It is a chained hash table with power-of-two bucket counts
and *incremental rehashing*: growing keeps both the old and new bucket arrays
alive and migrates a few buckets per operation, so no single command ever pays
for a full table resize. Power-of-two sizing is also what makes the
reverse-binary `SCAN` cursor work — libstdc++ uses prime bucket counts, so the
guarantee `SCAN` depends on cannot be built on top of it. One dict backs the
keyspace, the expiry table, hashes, sets and sorted-set scores, so
`SCAN`/`HSCAN`/`SSCAN`/`ZSCAN` all inherit the same behaviour:

> an element present for the whole iteration is returned at least once;
> elements may be returned more than once.

**[`RespReader`](src/resp/reader.h)** is genuinely incremental. A command may
arrive one byte at a time and the parser keeps its position between calls
instead of re-scanning the buffer. It handles both the multibulk framing real
clients send and the inline form a terminal sends, and enforces Redis's size
limits. It is the only code reachable before anything else, so it is fuzzed.

**[`SkipList`](src/store/skiplist.h)** orders sorted sets. Every forward pointer
carries a `span` — the number of nodes it jumps over — which is what turns
`ZRANK` and rank-based ranges into O(log n) instead of O(n). It is checked
against a sorted-vector oracle across thousands of randomized operations.

**Expiration** is the same two-pronged strategy Redis uses: *lazily* on every
lookup, so an expired key can never be observed, and *actively* from `serverCron`
by random sampling, so untouched keys are still reclaimed. The sampler keeps
going while more than a quarter of a sample came back expired, bounded by a time
budget so a cron tick cannot stall the loop.

## Commands

| Group | Commands |
|---|---|
| Connection/server | `PING` `ECHO` `SELECT` `SWAPDB` `HELLO` `QUIT` `RESET` `COMMAND` `INFO` `CONFIG` `DBSIZE` `FLUSHDB` `FLUSHALL` `TIME` `CLIENT` `SHUTDOWN` `DEBUG` `LOLWUT` |
| Generic | `DEL` `UNLINK` `EXISTS` `TYPE` `KEYS` `SCAN` `RANDOMKEY` `RENAME` `RENAMENX` `COPY` `TOUCH` `OBJECT` |
| Expiration | `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` `EXPIRETIME` `PEXPIRETIME` `PERSIST` |
| String | `SET` (`EX`/`PX`/`EXAT`/`PXAT`/`NX`/`XX`/`KEEPTTL`/`GET`) `GET` `GETSET` `GETDEL` `GETEX` `SETNX` `SETEX` `PSETEX` `MSET` `MSETNX` `MGET` `APPEND` `STRLEN` `GETRANGE` `SETRANGE` `INCR` `DECR` `INCRBY` `DECRBY` `INCRBYFLOAT` |
| List | `LPUSH` `RPUSH` `LPUSHX` `RPUSHX` `LPOP` `RPOP` `LRANGE` `LLEN` `LINDEX` `LSET` `LREM` `LTRIM` `LINSERT` `LPOS` `RPOPLPUSH` `LMOVE` `LMPOP` |
| Hash | `HSET` `HMSET` `HSETNX` `HGET` `HMGET` `HDEL` `HLEN` `HEXISTS` `HSTRLEN` `HKEYS` `HVALS` `HGETALL` `HINCRBY` `HINCRBYFLOAT` `HRANDFIELD` `HSCAN` |
| Set | `SADD` `SREM` `SCARD` `SISMEMBER` `SMISMEMBER` `SMEMBERS` `SPOP` `SRANDMEMBER` `SMOVE` `SUNION` `SINTER` `SDIFF` (+`STORE`) `SINTERCARD` `SSCAN` |
| Sorted set | `ZADD` `ZINCRBY` `ZSCORE` `ZMSCORE` `ZCARD` `ZREM` `ZRANK` `ZREVRANK` `ZCOUNT` `ZLEXCOUNT` `ZRANGE` `ZREVRANGE` `ZRANGEBYSCORE` `ZREVRANGEBYSCORE` `ZRANGEBYLEX` `ZREVRANGEBYLEX` `ZRANGESTORE` `ZREMRANGEBYRANK\|BYSCORE\|BYLEX` `ZPOPMIN` `ZPOPMAX` `ZMPOP` `ZRANDMEMBER` `ZUNION` `ZINTER` `ZDIFF` (+`STORE`) `ZINTERCARD` `ZSCAN` |

Error strings are copied verbatim from Redis — `WRONGTYPE Operation against a key
holding the wrong kind of value`, `ERR value is not an integer or out of range`,
and the rest — so client libraries that branch on them behave normally.

Both RESP2 and RESP3 are supported. `HELLO 3` switches a connection over, after
which nulls, maps, sets and doubles use the RESP3 encodings.

## Not implemented

Deliberately out of scope for this version: **persistence** (RDB/AOF),
**replication**, **pub/sub**, **transactions** (`MULTI`/`EXEC`), **blocking
commands** (`BLPOP`/`BZPOPMIN`), **Lua scripting**, **streams**, **cluster**,
**`AUTH`/ACL** and **TLS**.

The hooks these need already exist: every command carries a `kCmdWrite` flag and
each write calls a no-op `Server::propagate()`, which is where an AOF writer or a
replication stream would attach without touching the command implementations.

## Differences from real Redis

These are choices, not oversights:

- **Container encodings.** Redis switches representation by size — listpack for
  small hashes and sorted sets, intset for all-integer sets, quicklist for lists.
  credis uses one representation per type: `std::deque` for lists, the dict for
  hashes and sets, dict + skiplist for sorted sets. Small collections therefore
  use more memory than Redis would. `OBJECT ENCODING` reports the familiar names
  so clients see expected answers, but it is describing credis's structures.
- **Version reporting.** `HELLO` and `INFO` report `redis_version:7.4.0` so
  client libraries that gate features on the version behave normally. `INFO` also
  reports the true `credis_version`.
- **`glob_match("*", "")`.** Redis's `stringmatchlen` returns no-match for an
  empty subject and papers over it with an "all keys" shortcut in `KEYS`. credis
  handles the empty subject in the matcher itself, so an all-`*` pattern matches
  the empty key everywhere.
- **`UNLINK`** is identical to `DEL`; there is no background free thread to hand
  the memory to.
- **Memory accounting.** `INFO memory` reports zeros — there is no allocator
  instrumentation, and `maxmemory` eviction is not implemented.

## Testing

Unit tests cover the parts with sharp edges, and the end-to-end suite covers
everything that only shows up over a real socket.

- `Dict` — insert/erase/lookup, forced rehash, and a property test that a key
  present throughout an iteration is never missed by `SCAN` while the table is
  churning underneath it.
- `RespReader` — byte-at-a-time delivery, every malformed input, size limits, a
  20,000-iteration fuzz pass, and a check that chunked delivery parses identically
  to whole delivery.
- `SkipList` — cross-checked against a sorted-vector oracle over thousands of
  randomized inserts, deletes and score updates, verifying order, both traversal
  directions, and every span-derived rank.
- `EventLoop` — readiness dispatch, periodic timers, deferred destruction.
- `integration.py` — 200+ checks over a real connection: every command group,
  pipelining, 8MB replies (which exercise the partial-write path), split packets,
  protocol errors, RESP3, active expiry, and connection isolation.

### Throughput

Pipelined, 50 clients, on a 20-core Linux box (`-O1` build; `Release` is faster):

| Command | ops/sec |
|---|---|
| `INCR` | ~1.47M |
| `LPUSH` | ~1.41M |
| `SADD` | ~1.32M |
| `HSET` | ~1.23M |
| `GET` | ~0.91M |
| `SET` | ~0.65M |
| `ZADD` | ~0.50M |
