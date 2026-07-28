#include "commands/table.h"

#include <format>
#include <unordered_map>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "util/strings.h"

namespace credis {
namespace {

// name, proc, arity, flags, first_key, last_key, key_step
const std::vector<Command>& command_table() {
  static const std::vector<Command> table = {
      // --- connection / server ---
      {"ping", cmd_ping, -1, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"echo", cmd_echo, 2, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"select", cmd_select, 2, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"swapdb", cmd_swapdb, 3, kCmdWrite | kCmdFast | kCmdNoDb, 0, 0, 0},
      {"hello", cmd_hello, -1, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"quit", cmd_quit, -1, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"reset", cmd_reset, 1, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"command", cmd_command, -1, kCmdNoDb, 0, 0, 0},
      {"config", cmd_config, -2, kCmdAdmin | kCmdNoDb, 0, 0, 0},
      {"dbsize", cmd_dbsize, 1, kCmdReadonly | kCmdFast, 0, 0, 0},
      {"flushdb", cmd_flushdb, -1, kCmdWrite, 0, 0, 0},
      {"flushall", cmd_flushall, -1, kCmdWrite, 0, 0, 0},
      {"time", cmd_time, 1, kCmdFast | kCmdNoDb, 0, 0, 0},
      {"client", cmd_client, -2, kCmdAdmin | kCmdNoDb, 0, 0, 0},
      {"shutdown", cmd_shutdown, -1, kCmdAdmin | kCmdNoDb, 0, 0, 0},
      {"info", cmd_info, -1, kCmdNoDb, 0, 0, 0},
      {"debug", cmd_debug, -2, kCmdAdmin, 0, 0, 0},
      {"lolwut", cmd_lolwut, -1, kCmdReadonly | kCmdNoDb, 0, 0, 0},

      // --- generic keyspace ---
      {"del", cmd_del, -2, kCmdWrite, 1, -1, 1},
      {"unlink", cmd_unlink, -2, kCmdWrite | kCmdFast, 1, -1, 1},
      {"exists", cmd_exists, -2, kCmdReadonly | kCmdFast, 1, -1, 1},
      {"type", cmd_type, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"keys", cmd_keys, 2, kCmdReadonly, 0, 0, 0},
      {"scan", cmd_scan, -2, kCmdReadonly, 0, 0, 0},
      {"randomkey", cmd_randomkey, 1, kCmdReadonly, 0, 0, 0},
      {"rename", cmd_rename, 3, kCmdWrite, 1, 2, 1},
      {"renamenx", cmd_renamenx, 3, kCmdWrite | kCmdFast, 1, 2, 1},
      {"copy", cmd_copy, -3, kCmdWrite, 1, 2, 1},
      {"touch", cmd_touch, -2, kCmdReadonly | kCmdFast, 1, -1, 1},
      {"object", cmd_object, -2, kCmdReadonly, 2, 2, 1},

      // --- expiration ---
      {"expire", cmd_expire, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"pexpire", cmd_pexpire, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"expireat", cmd_expireat, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"pexpireat", cmd_pexpireat, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"ttl", cmd_ttl, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"pttl", cmd_pttl, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"expiretime", cmd_expiretime, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"pexpiretime", cmd_pexpiretime, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"persist", cmd_persist, 2, kCmdWrite | kCmdFast, 1, 1, 1},

      // --- strings ---
      {"set", cmd_set, -3, kCmdWrite, 1, 1, 1},
      {"setnx", cmd_setnx, 3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"setex", cmd_setex, 4, kCmdWrite, 1, 1, 1},
      {"psetex", cmd_psetex, 4, kCmdWrite, 1, 1, 1},
      {"get", cmd_get, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"getset", cmd_getset, 3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"getdel", cmd_getdel, 2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"getex", cmd_getex, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"mset", cmd_mset, -3, kCmdWrite, 1, -1, 2},
      {"msetnx", cmd_msetnx, -3, kCmdWrite, 1, -1, 2},
      {"mget", cmd_mget, -2, kCmdReadonly | kCmdFast, 1, -1, 1},
      {"append", cmd_append, 3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"strlen", cmd_strlen, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"getrange", cmd_getrange, 4, kCmdReadonly, 1, 1, 1},
      {"setrange", cmd_setrange, 4, kCmdWrite, 1, 1, 1},
      {"incr", cmd_incr, 2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"decr", cmd_decr, 2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"incrby", cmd_incrby, 3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"decrby", cmd_decrby, 3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"incrbyfloat", cmd_incrbyfloat, 3, kCmdWrite | kCmdFast, 1, 1, 1},

      // --- lists ---
      {"lpush", cmd_lpush, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"rpush", cmd_rpush, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"lpushx", cmd_lpushx, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"rpushx", cmd_rpushx, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"lpop", cmd_lpop, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"rpop", cmd_rpop, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"llen", cmd_llen, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"lrange", cmd_lrange, 4, kCmdReadonly, 1, 1, 1},
      {"lindex", cmd_lindex, 3, kCmdReadonly, 1, 1, 1},
      {"lset", cmd_lset, 4, kCmdWrite, 1, 1, 1},
      {"lrem", cmd_lrem, 4, kCmdWrite, 1, 1, 1},
      {"ltrim", cmd_ltrim, 4, kCmdWrite, 1, 1, 1},
      {"linsert", cmd_linsert, 5, kCmdWrite, 1, 1, 1},
      {"lpos", cmd_lpos, -3, kCmdReadonly, 1, 1, 1},
      {"rpoplpush", cmd_rpoplpush, 3, kCmdWrite, 1, 2, 1},
      {"lmove", cmd_lmove, 5, kCmdWrite, 1, 2, 1},
      {"lmpop", cmd_lmpop, -4, kCmdWrite, 0, 0, 0},

      // --- hashes ---
      {"hset", cmd_hset, -4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hmset", cmd_hmset, -4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hsetnx", cmd_hsetnx, 4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hget", cmd_hget, 3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"hmget", cmd_hmget, -3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"hdel", cmd_hdel, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hlen", cmd_hlen, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"hexists", cmd_hexists, 3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"hstrlen", cmd_hstrlen, 3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"hkeys", cmd_hkeys, 2, kCmdReadonly, 1, 1, 1},
      {"hvals", cmd_hvals, 2, kCmdReadonly, 1, 1, 1},
      {"hgetall", cmd_hgetall, 2, kCmdReadonly, 1, 1, 1},
      {"hincrby", cmd_hincrby, 4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hincrbyfloat", cmd_hincrbyfloat, 4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"hrandfield", cmd_hrandfield, -2, kCmdReadonly, 1, 1, 1},
      {"hscan", cmd_hscan, -3, kCmdReadonly, 1, 1, 1},

      // --- sets ---
      {"sadd", cmd_sadd, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"srem", cmd_srem, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"scard", cmd_scard, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"sismember", cmd_sismember, 3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"smismember", cmd_smismember, -3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"smembers", cmd_smembers, 2, kCmdReadonly, 1, 1, 1},
      {"spop", cmd_spop, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"srandmember", cmd_srandmember, -2, kCmdReadonly, 1, 1, 1},
      {"smove", cmd_smove, 4, kCmdWrite | kCmdFast, 1, 2, 1},
      {"sunion", cmd_sunion, -2, kCmdReadonly, 1, -1, 1},
      {"sinter", cmd_sinter, -2, kCmdReadonly, 1, -1, 1},
      {"sdiff", cmd_sdiff, -2, kCmdReadonly, 1, -1, 1},
      {"sunionstore", cmd_sunionstore, -3, kCmdWrite, 1, -1, 1},
      {"sinterstore", cmd_sinterstore, -3, kCmdWrite, 1, -1, 1},
      {"sdiffstore", cmd_sdiffstore, -3, kCmdWrite, 1, -1, 1},
      {"sintercard", cmd_sintercard, -3, kCmdReadonly, 0, 0, 0},
      {"sscan", cmd_sscan, -3, kCmdReadonly, 1, 1, 1},

      // --- sorted sets ---
      {"zadd", cmd_zadd, -4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"zincrby", cmd_zincrby, 4, kCmdWrite | kCmdFast, 1, 1, 1},
      {"zscore", cmd_zscore, 3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zmscore", cmd_zmscore, -3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zcard", cmd_zcard, 2, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zrem", cmd_zrem, -3, kCmdWrite | kCmdFast, 1, 1, 1},
      {"zrank", cmd_zrank, -3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zrevrank", cmd_zrevrank, -3, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zcount", cmd_zcount, 4, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zlexcount", cmd_zlexcount, 4, kCmdReadonly | kCmdFast, 1, 1, 1},
      {"zrange", cmd_zrange, -4, kCmdReadonly, 1, 1, 1},
      {"zrevrange", cmd_zrevrange, -4, kCmdReadonly, 1, 1, 1},
      {"zrangebyscore", cmd_zrangebyscore, -4, kCmdReadonly, 1, 1, 1},
      {"zrevrangebyscore", cmd_zrevrangebyscore, -4, kCmdReadonly, 1, 1, 1},
      {"zrangebylex", cmd_zrangebylex, -4, kCmdReadonly, 1, 1, 1},
      {"zrevrangebylex", cmd_zrevrangebylex, -4, kCmdReadonly, 1, 1, 1},
      {"zrangestore", cmd_zrangestore, -5, kCmdWrite, 1, 2, 1},
      {"zremrangebyrank", cmd_zremrangebyrank, 4, kCmdWrite, 1, 1, 1},
      {"zremrangebyscore", cmd_zremrangebyscore, 4, kCmdWrite, 1, 1, 1},
      {"zremrangebylex", cmd_zremrangebylex, 4, kCmdWrite, 1, 1, 1},
      {"zpopmin", cmd_zpopmin, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"zpopmax", cmd_zpopmax, -2, kCmdWrite | kCmdFast, 1, 1, 1},
      {"zmpop", cmd_zmpop, -4, kCmdWrite, 0, 0, 0},
      {"zrandmember", cmd_zrandmember, -2, kCmdReadonly, 1, 1, 1},
      {"zunion", cmd_zunion, -3, kCmdReadonly, 0, 0, 0},
      {"zinter", cmd_zinter, -3, kCmdReadonly, 0, 0, 0},
      {"zdiff", cmd_zdiff, -3, kCmdReadonly, 0, 0, 0},
      {"zunionstore", cmd_zunionstore, -4, kCmdWrite, 1, 1, 1},
      {"zinterstore", cmd_zinterstore, -4, kCmdWrite, 1, 1, 1},
      {"zdiffstore", cmd_zdiffstore, -4, kCmdWrite, 1, 1, 1},
      {"zintercard", cmd_zintercard, -3, kCmdReadonly, 0, 0, 0},
      {"zscan", cmd_zscan, -3, kCmdReadonly, 1, 1, 1},
  };
  return table;
}

const std::unordered_map<std::string_view, const Command*>& command_index() {
  static const auto index = [] {
    std::unordered_map<std::string_view, const Command*> map;
    for (const Command& command : command_table()) map.emplace(command.name, &command);
    return map;
  }();
  return index;
}

}  // namespace

const std::vector<Command>& all_commands() { return command_table(); }

const Command* lookup_command(std::string_view name) {
  // Command names are ASCII and short; lowercasing into a stack buffer keeps the
  // dispatch path free of allocations.
  char buf[64];
  if (name.size() >= sizeof(buf)) return nullptr;
  for (size_t i = 0; i < name.size(); ++i) buf[i] = lower_ascii(name[i]);

  const auto& index = command_index();
  const auto it = index.find(std::string_view(buf, name.size()));
  return it == index.end() ? nullptr : it->second;
}

bool arity_ok(const Command& command, size_t argc) {
  const int count = static_cast<int>(argc);
  if (command.arity > 0) return count == command.arity;
  return count >= -command.arity;
}

void reply_wrong_type(Client& client) { client.out().error(err::kWrongType); }

void reply_wrong_args(Client& client, std::string_view command_name) {
  client.out().error(
      std::format("ERR wrong number of arguments for '{}' command", command_name));
}

void reply_unknown_subcommand(Client& client, std::string_view container,
                              std::string_view subcommand) {
  client.out().error(std::format(
      "ERR Unknown {} subcommand or wrong number of arguments for '{}'. Try {} HELP.", container,
      subcommand, to_upper(container)));
}

}  // namespace credis
