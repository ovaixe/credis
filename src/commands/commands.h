#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "commands/table.h"
#include "store/object.h"

namespace credis {

// --- shared argument helpers --------------------------------------------------
// Each replies with the appropriate error and returns false on bad input, so
// command bodies read as a sequence of guarded parses.

bool get_int64_arg(Client& client, std::string_view value, int64_t* out,
                   std::string_view error = err::kNotInteger);
bool get_double_arg(Client& client, std::string_view value, double* out,
                    std::string_view error = err::kNotFloat);

// Verifies an existing object's type. A null object passes (the key is simply
// absent); a mismatch replies WRONGTYPE and returns false.
bool check_type(Client& client, const Object* object, ObjectType expected);

// Records a keyspace hit or miss for INFO.
void note_lookup(Client& client, bool found);

// Deletes `key` if it now holds an empty aggregate — Redis never keeps a
// zero-element list, hash, set or sorted set around.
void delete_if_empty(Client& client, std::string_view key, const Object* object);

// Normalizes a possibly-negative range index against a container of `length`
// elements, the way LRANGE/GETRANGE/ZRANGE do.
void normalize_range(int64_t length, int64_t* start, int64_t* end);

// Options shared by SCAN, HSCAN, SSCAN and ZSCAN.
struct ScanOptions {
  uint64_t cursor = 0;
  int64_t count = 10;
  std::string pattern;
  bool has_pattern = false;
  bool novalues = false;  // HSCAN NOVALUES
};

// Parses "<cursor> [MATCH p] [COUNT n] [NOVALUES]" starting at argv[1], with
// options from argv[first_option]. Replies with the error itself on bad input.
bool parse_scan_options(Client& client, size_t first_option, bool allow_novalues,
                        ScanOptions* out);

// True when the scan pattern accepts everything, so the matcher can be skipped.
bool scan_matches(const ScanOptions& options, std::string_view key);

// --- connection and server ----------------------------------------------------
void cmd_ping(Client&);
void cmd_echo(Client&);
void cmd_select(Client&);
void cmd_swapdb(Client&);
void cmd_hello(Client&);
void cmd_quit(Client&);
void cmd_reset(Client&);
void cmd_command(Client&);
void cmd_config(Client&);
void cmd_dbsize(Client&);
void cmd_flushdb(Client&);
void cmd_flushall(Client&);
void cmd_time(Client&);
void cmd_client(Client&);
void cmd_shutdown(Client&);
void cmd_info(Client&);
void cmd_debug(Client&);
void cmd_lolwut(Client&);

// --- generic keyspace ---------------------------------------------------------
void cmd_del(Client&);
void cmd_unlink(Client&);
void cmd_exists(Client&);
void cmd_type(Client&);
void cmd_keys(Client&);
void cmd_scan(Client&);
void cmd_randomkey(Client&);
void cmd_rename(Client&);
void cmd_renamenx(Client&);
void cmd_copy(Client&);
void cmd_touch(Client&);
void cmd_object(Client&);

// --- expiration ---------------------------------------------------------------
void cmd_expire(Client&);
void cmd_pexpire(Client&);
void cmd_expireat(Client&);
void cmd_pexpireat(Client&);
void cmd_ttl(Client&);
void cmd_pttl(Client&);
void cmd_expiretime(Client&);
void cmd_pexpiretime(Client&);
void cmd_persist(Client&);

// --- strings ------------------------------------------------------------------
void cmd_set(Client&);
void cmd_setnx(Client&);
void cmd_setex(Client&);
void cmd_psetex(Client&);
void cmd_get(Client&);
void cmd_getset(Client&);
void cmd_getdel(Client&);
void cmd_getex(Client&);
void cmd_mset(Client&);
void cmd_msetnx(Client&);
void cmd_mget(Client&);
void cmd_append(Client&);
void cmd_strlen(Client&);
void cmd_getrange(Client&);
void cmd_setrange(Client&);
void cmd_incr(Client&);
void cmd_decr(Client&);
void cmd_incrby(Client&);
void cmd_decrby(Client&);
void cmd_incrbyfloat(Client&);

// --- lists --------------------------------------------------------------------
void cmd_lpush(Client&);
void cmd_rpush(Client&);
void cmd_lpushx(Client&);
void cmd_rpushx(Client&);
void cmd_lpop(Client&);
void cmd_rpop(Client&);
void cmd_llen(Client&);
void cmd_lrange(Client&);
void cmd_lindex(Client&);
void cmd_lset(Client&);
void cmd_lrem(Client&);
void cmd_ltrim(Client&);
void cmd_linsert(Client&);
void cmd_lpos(Client&);
void cmd_rpoplpush(Client&);
void cmd_lmove(Client&);
void cmd_lmpop(Client&);

// --- hashes -------------------------------------------------------------------
void cmd_hset(Client&);
void cmd_hmset(Client&);
void cmd_hsetnx(Client&);
void cmd_hget(Client&);
void cmd_hmget(Client&);
void cmd_hdel(Client&);
void cmd_hlen(Client&);
void cmd_hexists(Client&);
void cmd_hstrlen(Client&);
void cmd_hkeys(Client&);
void cmd_hvals(Client&);
void cmd_hgetall(Client&);
void cmd_hincrby(Client&);
void cmd_hincrbyfloat(Client&);
void cmd_hrandfield(Client&);
void cmd_hscan(Client&);

// --- sets ---------------------------------------------------------------------
void cmd_sadd(Client&);
void cmd_srem(Client&);
void cmd_scard(Client&);
void cmd_sismember(Client&);
void cmd_smismember(Client&);
void cmd_smembers(Client&);
void cmd_spop(Client&);
void cmd_srandmember(Client&);
void cmd_smove(Client&);
void cmd_sunion(Client&);
void cmd_sinter(Client&);
void cmd_sdiff(Client&);
void cmd_sunionstore(Client&);
void cmd_sinterstore(Client&);
void cmd_sdiffstore(Client&);
void cmd_sintercard(Client&);
void cmd_sscan(Client&);

// --- sorted sets --------------------------------------------------------------
void cmd_zadd(Client&);
void cmd_zincrby(Client&);
void cmd_zscore(Client&);
void cmd_zmscore(Client&);
void cmd_zcard(Client&);
void cmd_zrem(Client&);
void cmd_zrank(Client&);
void cmd_zrevrank(Client&);
void cmd_zcount(Client&);
void cmd_zlexcount(Client&);
void cmd_zrange(Client&);
void cmd_zrevrange(Client&);
void cmd_zrangebyscore(Client&);
void cmd_zrevrangebyscore(Client&);
void cmd_zrangebylex(Client&);
void cmd_zrevrangebylex(Client&);
void cmd_zrangestore(Client&);
void cmd_zremrangebyrank(Client&);
void cmd_zremrangebyscore(Client&);
void cmd_zremrangebylex(Client&);
void cmd_zpopmin(Client&);
void cmd_zpopmax(Client&);
void cmd_zmpop(Client&);
void cmd_zrandmember(Client&);
void cmd_zunion(Client&);
void cmd_zinter(Client&);
void cmd_zdiff(Client&);
void cmd_zunionstore(Client&);
void cmd_zinterstore(Client&);
void cmd_zdiffstore(Client&);
void cmd_zintercard(Client&);
void cmd_zscan(Client&);

}  // namespace credis
