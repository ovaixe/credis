#include <unistd.h>

#include <format>
#include <string>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "util/strings.h"
#include "util/time.h"

namespace credis {
namespace {

// Reported to clients so that libraries which gate features on the server
// version behave as they would against a real Redis. INFO also reports
// credis_version, which is the honest one.
constexpr std::string_view kEmulatedRedisVersion = "7.4.0";
constexpr std::string_view kCredisVersion = "0.1.0";

bool parse_db_index(Client& client, std::string_view value, int* out,
                    std::string_view parse_error) {
  int64_t index = 0;
  if (!string2ll(value, &index)) {
    client.out().error(parse_error);
    return false;
  }
  if (index < 0 || index >= client.server().db_count()) {
    client.out().error(err::kSelectOutOfRange);
    return false;
  }
  *out = static_cast<int>(index);
  return true;
}

}  // namespace

void cmd_ping(Client& client) {
  if (client.argc() == 1) {
    client.out().simple_string("PONG");
  } else if (client.argc() == 2) {
    client.out().bulk(client.arg(1));
  } else {
    reply_wrong_args(client, "ping");
  }
}

void cmd_echo(Client& client) { client.out().bulk(client.arg(1)); }

void cmd_select(Client& client) {
  int index = 0;
  if (!parse_db_index(client, client.arg(1), &index, "ERR invalid DB index")) return;
  client.select_db(index);
  client.out().ok();
}

void cmd_swapdb(Client& client) {
  int first = 0;
  int second = 0;
  if (!parse_db_index(client, client.arg(1), &first, "ERR invalid first DB index")) return;
  if (!parse_db_index(client, client.arg(2), &second, "ERR invalid second DB index")) return;
  client.server().swap_dbs(first, second);
  client.out().ok();
}

void cmd_hello(Client& client) {
  size_t next = 1;
  if (client.argc() > 1) {
    int64_t version = 0;
    if (!string2ll(client.arg(1), &version) || version < 2 || version > 3) {
      client.out().error("NOPROTO unsupported protocol version");
      return;
    }
    client.set_protocol(version == 3 ? RespProtocol::Resp3 : RespProtocol::Resp2);
    next = 2;
  }

  // Remaining options: [AUTH user pass] [SETNAME name]. credis has no auth, so
  // AUTH is accepted and ignored rather than failing clients that always send it.
  while (next < client.argc()) {
    const std::string& option = client.arg(next);
    if (str_ieq(option, "auth") && next + 2 < client.argc()) {
      next += 3;
    } else if (str_ieq(option, "setname") && next + 1 < client.argc()) {
      client.set_name(client.arg(next + 1));
      next += 2;
    } else {
      client.out().error(std::format(
          "ERR Protocol error, got '{}' as reply type byte", option.empty() ? ' ' : option[0]));
      return;
    }
  }

  RespWriter& out = client.out();
  out.map(7);
  out.bulk("server");
  out.bulk("redis");
  out.bulk("version");
  out.bulk(kEmulatedRedisVersion);
  out.bulk("proto");
  out.integer(static_cast<int64_t>(client.protocol()));
  out.bulk("id");
  out.integer(static_cast<int64_t>(client.id()));
  out.bulk("mode");
  out.bulk("standalone");
  out.bulk("role");
  out.bulk("master");
  out.bulk("modules");
  out.array(0);
}

void cmd_quit(Client& client) {
  client.out().ok();
  client.close_after_reply();
}

void cmd_reset(Client& client) {
  client.reset_state();
  client.out().simple_string("RESET");
}

void cmd_dbsize(Client& client) {
  client.out().integer(static_cast<int64_t>(client.db().size()));
}

void cmd_flushdb(Client& client) {
  // ASYNC/SYNC are accepted for compatibility; credis frees synchronously.
  if (client.argc() > 2 ||
      (client.argc() == 2 && !str_ieq(client.arg(1), "async") &&
       !str_ieq(client.arg(1), "sync"))) {
    client.out().error(err::kSyntax);
    return;
  }
  client.db().clear();
  client.out().ok();
}

void cmd_flushall(Client& client) {
  if (client.argc() > 2 ||
      (client.argc() == 2 && !str_ieq(client.arg(1), "async") &&
       !str_ieq(client.arg(1), "sync"))) {
    client.out().error(err::kSyntax);
    return;
  }
  for (int i = 0; i < client.server().db_count(); ++i) client.server().db(i).clear();
  client.out().ok();
}

void cmd_time(Client& client) {
  const int64_t now = ustime();
  client.out().array(2);
  client.out().bulk(ll2string(now / 1000000));
  client.out().bulk(ll2string(now % 1000000));
}

void cmd_lolwut(Client& client) {
  client.out().bulk(std::format("credis {} — a Redis server written in C++\n", kCredisVersion));
}

void cmd_shutdown(Client& client) {
  // NOSAVE/SAVE/NOW/FORCE are accepted; credis has no persistence to flush.
  client.server().stop();
}

// --- COMMAND ------------------------------------------------------------------

namespace {

void reply_command_flags(RespWriter& out, const Command& command) {
  // Count first: the array header has to precede the elements.
  int count = 0;
  if (command.flags & kCmdWrite) ++count;
  if (command.flags & kCmdReadonly) ++count;
  if (command.flags & kCmdAdmin) ++count;
  if (command.flags & kCmdFast) ++count;

  out.array(count);
  if (command.flags & kCmdWrite) out.simple_string("write");
  if (command.flags & kCmdReadonly) out.simple_string("readonly");
  if (command.flags & kCmdAdmin) out.simple_string("admin");
  if (command.flags & kCmdFast) out.simple_string("fast");
}

// The 10-element per-command reply shape Redis 7 uses. The trailing four are
// introspection details credis does not model, and are sent empty.
void reply_command_info(RespWriter& out, const Command& command) {
  out.array(10);
  out.bulk(command.name);
  out.integer(command.arity);
  reply_command_flags(out, command);
  out.integer(command.first_key);
  out.integer(command.last_key);
  out.integer(command.key_step);
  out.array(0);  // ACL categories
  out.array(0);  // tips
  out.array(0);  // key specifications
  out.array(0);  // subcommands
}

}  // namespace

void cmd_command(Client& client) {
  RespWriter& out = client.out();

  if (client.argc() == 1) {
    out.array(static_cast<int64_t>(all_commands().size()));
    for (const Command& command : all_commands()) reply_command_info(out, command);
    return;
  }

  const std::string& subcommand = client.arg(1);
  if (str_ieq(subcommand, "count")) {
    out.integer(static_cast<int64_t>(all_commands().size()));
    return;
  }
  if (str_ieq(subcommand, "docs")) {
    // redis-cli issues COMMAND DOCS on connect. An empty map is a valid answer
    // and keeps the handshake quiet.
    out.map(0);
    return;
  }
  if (str_ieq(subcommand, "info")) {
    if (client.argc() == 2) {
      out.array(static_cast<int64_t>(all_commands().size()));
      for (const Command& command : all_commands()) reply_command_info(out, command);
      return;
    }
    out.array(static_cast<int64_t>(client.argc() - 2));
    for (size_t i = 2; i < client.argc(); ++i) {
      const Command* command = lookup_command(client.arg(i));
      if (command == nullptr) {
        out.null_array();
      } else {
        reply_command_info(out, *command);
      }
    }
    return;
  }
  if (str_ieq(subcommand, "list")) {
    out.array(static_cast<int64_t>(all_commands().size()));
    for (const Command& command : all_commands()) out.bulk(command.name);
    return;
  }

  reply_unknown_subcommand(client, "COMMAND", subcommand);
}

// --- CONFIG -------------------------------------------------------------------

namespace {

// The configuration surface credis exposes. Values are produced on demand so
// CONFIG GET always reflects the running server.
std::string config_value(const Server& server, std::string_view name) {
  const ServerConfig& config = server.config();
  if (name == "maxmemory") return "0";
  if (name == "maxmemory-policy") return "noeviction";
  if (name == "maxclients") return ll2string(static_cast<int64_t>(config.maxclients));
  if (name == "databases") return ll2string(config.databases);
  if (name == "port") return ll2string(config.port);
  if (name == "bind") return config.bind_addr;
  if (name == "dir") return config.dir;
  if (name == "logfile") return config.logfile;
  if (name == "hz") return ll2string(config.hz);
  if (name == "appendonly") return "no";
  if (name == "save") return "";
  if (name == "timeout") return "0";
  if (name == "tcp-keepalive") return "300";
  if (name == "tcp-backlog") return ll2string(config.tcp_backlog);
  if (name == "proto-max-bulk-len") return ll2string(kMaxBulkLen);
  if (name == "list-max-listpack-size") return "128";
  if (name == "hash-max-listpack-entries") return "128";
  if (name == "set-max-intset-entries") return "512";
  if (name == "zset-max-listpack-entries") return "128";
  if (name == "loglevel") {
    switch (config.loglevel) {
      case LogLevel::Debug: return "debug";
      case LogLevel::Verbose: return "verbose";
      case LogLevel::Notice: return "notice";
      case LogLevel::Warning: return "warning";
    }
  }
  return {};
}

constexpr std::string_view kConfigParameters[] = {
    "maxmemory",     "maxmemory-policy",          "maxclients",
    "databases",     "port",                      "bind",
    "dir",           "logfile",                   "hz",
    "appendonly",    "save",                      "timeout",
    "tcp-keepalive", "tcp-backlog",               "proto-max-bulk-len",
    "loglevel",      "list-max-listpack-size",    "hash-max-listpack-entries",
    "set-max-intset-entries",                     "zset-max-listpack-entries",
};

}  // namespace

void cmd_config(Client& client) {
  const std::string& subcommand = client.arg(1);
  RespWriter& out = client.out();

  if (str_ieq(subcommand, "get")) {
    if (client.argc() < 3) {
      reply_unknown_subcommand(client, "CONFIG", subcommand);
      return;
    }
    // Collect first: the map header needs the pair count up front, and a
    // parameter matching two patterns must still appear only once.
    std::vector<std::pair<std::string, std::string>> matches;
    for (std::string_view parameter : kConfigParameters) {
      bool matched = false;
      for (size_t i = 2; i < client.argc() && !matched; ++i) {
        matched = glob_match(to_lower(client.arg(i)), parameter, /*nocase=*/true);
      }
      if (matched) matches.emplace_back(parameter, config_value(client.server(), parameter));
    }

    out.map(static_cast<int64_t>(matches.size()));
    for (const auto& [name, value] : matches) {
      out.bulk(name);
      out.bulk(value);
    }
    return;
  }

  if (str_ieq(subcommand, "set")) {
    if (client.argc() < 4 || client.argc() % 2 != 0) {
      reply_wrong_args(client, "config|set");
      return;
    }
    // Only the settings that genuinely take effect are honoured; the rest are
    // accepted so client libraries' startup tuning does not fail.
    for (size_t i = 2; i + 1 < client.argc(); i += 2) {
      const std::string parameter = to_lower(client.arg(i));
      const std::string& value = client.arg(i + 1);
      if (parameter == "loglevel") {
        if (value == "debug") client.server().mutable_config().loglevel = LogLevel::Debug;
        else if (value == "verbose") client.server().mutable_config().loglevel = LogLevel::Verbose;
        else if (value == "notice") client.server().mutable_config().loglevel = LogLevel::Notice;
        else if (value == "warning") client.server().mutable_config().loglevel = LogLevel::Warning;
        else {
          out.error(std::format("ERR CONFIG SET failed - argument couldn't be parsed into an "
                                "integer or out of range for parameter '{}'",
                                parameter));
          return;
        }
        set_log_level(client.server().config().loglevel);
      }
    }
    out.ok();
    return;
  }

  if (str_ieq(subcommand, "resetstat")) {
    client.server().stats() = ServerStats{};
    out.ok();
    return;
  }

  if (str_ieq(subcommand, "rewrite")) {
    out.error("ERR The server is running without a config file");
    return;
  }

  reply_unknown_subcommand(client, "CONFIG", subcommand);
}

// --- CLIENT -------------------------------------------------------------------

namespace {

std::string describe_client(const Connection& connection, int64_t now_ms) {
  return std::format(
      "id={} addr={} laddr= fd={} name={} age={} idle={} flags=N db={} sub=0 psub=0 ssub=0 "
      "multi=-1 watch=0 qbuf=0 qbuf-free=0 argv-mem=0 multi-mem=0 tot-net-in=0 tot-net-out=0 "
      "rbs=1024 rbp=0 obl=0 oll=0 omem={} tot-mem=0 events=r cmd={} user=default redir=-1 "
      "resp={}",
      connection.id(), connection.peer(), connection.fd(), connection.name(),
      (now_ms - connection.created_ms()) / 1000, (now_ms - connection.last_interaction_ms()) / 1000,
      connection.db_index(), connection.output_bytes(), connection.last_command(),
      static_cast<int>(connection.protocol()));
}

}  // namespace

void cmd_client(Client& client) {
  const std::string& subcommand = client.arg(1);
  RespWriter& out = client.out();

  if (str_ieq(subcommand, "id") && client.argc() == 2) {
    out.integer(static_cast<int64_t>(client.id()));
    return;
  }
  if (str_ieq(subcommand, "getname") && client.argc() == 2) {
    if (client.name().empty()) {
      out.null_bulk();
    } else {
      out.bulk(client.name());
    }
    return;
  }
  if (str_ieq(subcommand, "setname") && client.argc() == 3) {
    const std::string& name = client.arg(2);
    // Redis forbids spaces and newlines so CLIENT LIST stays parseable.
    if (name.find(' ') != std::string::npos || name.find('\n') != std::string::npos) {
      out.error("ERR Client names cannot contain spaces, newlines or special characters.");
      return;
    }
    client.set_name(name);
    out.ok();
    return;
  }
  if (str_ieq(subcommand, "list")) {
    const int64_t now = mstime();
    std::string listing;
    for (const auto& [id, connection] : client.server().clients()) {
      (void)id;
      listing += describe_client(*connection, now);
      listing += '\n';
    }
    out.verbatim(listing);
    return;
  }
  if (str_ieq(subcommand, "info") && client.argc() == 2) {
    out.verbatim(describe_client(client, mstime()));
    return;
  }
  if (str_ieq(subcommand, "no-evict") || str_ieq(subcommand, "no-touch")) {
    out.ok();
    return;
  }
  if (str_ieq(subcommand, "kill")) {
    // Supported form: CLIENT KILL ID <id>. Killing by address would need the
    // connection registry to be indexed by peer, which credis does not do.
    if (client.argc() == 4 && str_ieq(client.arg(2), "id")) {
      int64_t id = 0;
      if (!get_int64_arg(client, client.arg(3), &id)) return;
      Connection* target = client.server().find_client(static_cast<uint64_t>(id));
      if (target == nullptr) {
        out.integer(0);
        return;
      }
      target->close_after_reply();
      target->flush_output();
      out.integer(1);
      return;
    }
    out.error("ERR syntax error");
    return;
  }

  reply_unknown_subcommand(client, "CLIENT", subcommand);
}

// --- INFO ---------------------------------------------------------------------

namespace {

bool wants_section(const Client& client, std::string_view section) {
  if (client.argc() == 1) return true;
  for (size_t i = 1; i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "all") || str_ieq(client.arg(i), "everything") ||
        str_ieq(client.arg(i), "default")) {
      return true;
    }
    if (str_ieq(client.arg(i), section)) return true;
  }
  return false;
}

}  // namespace

void cmd_info(Client& client) {
  Server& server = client.server();
  const ServerStats& stats = server.stats();
  const int64_t uptime_s = (mstime() - server.start_time_ms()) / 1000;
  std::string info;

  if (wants_section(client, "server")) {
    info += std::format(
        "# Server\r\n"
        "redis_version:{}\r\n"
        "credis_version:{}\r\n"
        "redis_mode:standalone\r\n"
        "os:Linux\r\n"
        "arch_bits:64\r\n"
        "process_id:{}\r\n"
        "tcp_port:{}\r\n"
        "uptime_in_seconds:{}\r\n"
        "uptime_in_days:{}\r\n"
        "executable:credis-server\r\n"
        "\r\n",
        kEmulatedRedisVersion, kCredisVersion, static_cast<int>(getpid()), server.port(), uptime_s,
        uptime_s / 86400);
  }

  if (wants_section(client, "clients")) {
    info += std::format(
        "# Clients\r\n"
        "connected_clients:{}\r\n"
        "maxclients:{}\r\n"
        "blocked_clients:0\r\n"
        "\r\n",
        server.client_count(), server.config().maxclients);
  }

  if (wants_section(client, "memory")) {
    info +=
        "# Memory\r\n"
        "used_memory:0\r\n"
        "used_memory_human:0B\r\n"
        "maxmemory:0\r\n"
        "maxmemory_policy:noeviction\r\n"
        "mem_allocator:libc\r\n"
        "\r\n";
  }

  if (wants_section(client, "persistence")) {
    info +=
        "# Persistence\r\n"
        "loading:0\r\n"
        "rdb_bgsave_in_progress:0\r\n"
        "rdb_last_bgsave_status:ok\r\n"
        "aof_enabled:0\r\n"
        "aof_last_write_status:ok\r\n"
        "\r\n";
  }

  if (wants_section(client, "stats")) {
    info += std::format(
        "# Stats\r\n"
        "total_connections_received:{}\r\n"
        "total_commands_processed:{}\r\n"
        "rejected_connections:{}\r\n"
        "total_net_input_bytes:{}\r\n"
        "total_net_output_bytes:{}\r\n"
        "expired_keys:{}\r\n"
        "keyspace_hits:{}\r\n"
        "keyspace_misses:{}\r\n"
        "\r\n",
        stats.connections_received, stats.commands_processed, stats.rejected_connections,
        stats.total_net_input_bytes, stats.total_net_output_bytes, stats.expired_keys,
        stats.keyspace_hits, stats.keyspace_misses);
  }

  if (wants_section(client, "replication")) {
    info +=
        "# Replication\r\n"
        "role:master\r\n"
        "connected_slaves:0\r\n"
        "\r\n";
  }

  if (wants_section(client, "cpu")) {
    info +=
        "# CPU\r\n"
        "used_cpu_sys:0.0\r\n"
        "used_cpu_user:0.0\r\n"
        "\r\n";
  }

  if (wants_section(client, "keyspace")) {
    info += "# Keyspace\r\n";
    for (int i = 0; i < server.db_count(); ++i) {
      const Db& database = server.db(i);
      if (database.size() == 0) continue;
      info += std::format("db{}:keys={},expires={},avg_ttl=0\r\n", i, database.size(),
                          database.expires().size());
    }
    info += "\r\n";
  }

  client.out().verbatim(info);
}

// --- DEBUG --------------------------------------------------------------------

void cmd_debug(Client& client) {
  const std::string& subcommand = client.arg(1);
  RespWriter& out = client.out();

  if (str_ieq(subcommand, "sleep") && client.argc() == 3) {
    double seconds = 0;
    if (!get_double_arg(client, client.arg(2), &seconds)) return;
    // Deliberately blocking: DEBUG SLEEP exists to stall the single-threaded
    // server, which is what tests use it for.
    ::usleep(static_cast<useconds_t>(seconds * 1000000));
    out.ok();
    return;
  }
  if (str_ieq(subcommand, "set-active-expire") && client.argc() == 3) {
    int64_t enabled = 0;
    if (!get_int64_arg(client, client.arg(2), &enabled)) return;
    client.server().mutable_config().active_expire = enabled != 0;
    out.ok();
    return;
  }
  if (str_ieq(subcommand, "jmap") || str_ieq(subcommand, "flushall")) {
    out.ok();
    return;
  }
  if (str_ieq(subcommand, "object") && client.argc() == 3) {
    const Object* object = client.db().lookup_read(client.arg(2));
    if (object == nullptr) {
      out.error(err::kNoSuchKey);
      return;
    }
    out.simple_string(std::format("Value at:0x0 refcount:1 encoding:{} serializedlength:{}",
                                  object->encoding(), object->element_count()));
    return;
  }
  if (str_ieq(subcommand, "stringmatch-len") && client.argc() == 4) {
    out.integer(glob_match(client.arg(2), client.arg(3)) ? 1 : 0);
    return;
  }

  reply_unknown_subcommand(client, "DEBUG", subcommand);
}

}  // namespace credis
