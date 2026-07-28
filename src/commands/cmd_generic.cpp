#include <format>
#include <limits>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"
#include "util/time.h"

namespace credis {
namespace {

int64_t delete_keys(Client& client) {
  int64_t deleted = 0;
  for (size_t i = 1; i < client.argc(); ++i) {
    if (client.db().erase(client.arg(i))) ++deleted;
  }
  return deleted;
}

}  // namespace

void cmd_del(Client& client) { client.out().integer(delete_keys(client)); }

// UNLINK differs from DEL only by freeing memory in a background thread; credis
// is single-threaded, so the two are the same operation.
void cmd_unlink(Client& client) { client.out().integer(delete_keys(client)); }

void cmd_exists(Client& client) {
  int64_t found = 0;
  // Repeating a key counts it repeatedly, matching Redis.
  for (size_t i = 1; i < client.argc(); ++i) {
    if (client.db().lookup_read(client.arg(i)) != nullptr) ++found;
  }
  client.out().integer(found);
}

void cmd_touch(Client& client) { cmd_exists(client); }

void cmd_type(Client& client) {
  const Object* object = client.db().lookup_read(client.arg(1));
  note_lookup(client, object != nullptr);
  client.out().simple_string(object == nullptr ? "none" : object->type_name());
}

void cmd_keys(Client& client) {
  const std::string& pattern = client.arg(1);
  // "*" is by far the most common pattern; skipping the matcher for it is the
  // same shortcut Redis takes.
  const bool all_keys = pattern == "*";

  std::vector<const std::string*> matches;
  Db& database = client.db();
  const int64_t now = mstime();

  database.dict().for_each([&](const std::string& key, const Object&) {
    // Expired-but-not-yet-collected keys must not be reported. They are only
    // skipped, not deleted: the active expiry cycle reclaims them, and leaving
    // the dict untouched keeps the collected pointers valid.
    const int64_t expire_at = database.get_expire(key);
    if (expire_at != Db::kNoExpire && expire_at <= now) return;
    if (all_keys || glob_match(pattern, key)) matches.push_back(&key);
  });

  client.out().array(static_cast<int64_t>(matches.size()));
  for (const std::string* key : matches) client.out().bulk(*key);
}

void cmd_randomkey(Client& client) {
  Db& database = client.db();
  if (database.size() == 0) {
    client.out().null_bulk();
    return;
  }

  // Sampling can keep landing on logically-expired keys; bound the attempts so a
  // keyspace that is entirely expired cannot spin.
  constexpr int kMaxAttempts = 100;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    auto* entry = database.dict().random_entry();
    if (entry == nullptr) break;
    const std::string key = entry->key;
    if (!database.expire_if_needed(key)) {
      client.out().bulk(key);
      return;
    }
  }
  client.out().null_bulk();
}

namespace {

void rename_generic(Client& client, bool nx) {
  Db& database = client.db();
  const std::string& source = client.arg(1);
  const std::string& destination = client.arg(2);

  Object* object = database.lookup_write(source);
  if (object == nullptr) {
    client.out().error(err::kNoSuchKey);
    return;
  }

  if (source == destination) {
    // Renaming onto itself is a no-op, but still reports success.
    if (nx) {
      client.out().integer(0);
    } else {
      client.out().ok();
    }
    return;
  }

  if (database.lookup_write(destination) != nullptr) {
    if (nx) {
      client.out().integer(0);
      return;
    }
    database.erase(destination);
    // The source entry may have been rehashed by the erase; look it up again.
    object = database.dict().find(source);
    if (object == nullptr) {
      client.out().error(err::kNoSuchKey);
      return;
    }
  }

  const int64_t expire_at = database.get_expire(source);
  Object moved = std::move(*object);
  database.erase(source);
  database.set(destination, std::move(moved));
  if (expire_at != Db::kNoExpire) database.set_expire(destination, expire_at);

  if (nx) {
    client.out().integer(1);
  } else {
    client.out().ok();
  }
}

}  // namespace

void cmd_rename(Client& client) { rename_generic(client, /*nx=*/false); }
void cmd_renamenx(Client& client) { rename_generic(client, /*nx=*/true); }

void cmd_copy(Client& client) {
  const std::string& source = client.arg(1);
  const std::string& destination = client.arg(2);

  int destination_db = client.db_index();
  bool replace = false;

  for (size_t i = 3; i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "replace")) {
      replace = true;
    } else if (str_ieq(client.arg(i), "db") && i + 1 < client.argc()) {
      int64_t index = 0;
      if (!get_int64_arg(client, client.arg(i + 1), &index)) return;
      if (index < 0 || index >= client.server().db_count()) {
        client.out().error(err::kSelectOutOfRange);
        return;
      }
      destination_db = static_cast<int>(index);
      ++i;
    } else {
      client.out().error(err::kSyntax);
      return;
    }
  }

  if (destination_db == client.db_index() && source == destination) {
    client.out().error(
        "ERR source and destination objects are the same");
    return;
  }

  Db& source_db = client.db();
  Db& target_db = client.server().db(destination_db);

  const Object* object = source_db.lookup_read(source);
  if (object == nullptr) {
    client.out().integer(0);
    return;
  }
  if (target_db.lookup_write(destination) != nullptr && !replace) {
    client.out().integer(0);
    return;
  }

  const int64_t expire_at = source_db.get_expire(source);
  target_db.set(destination, object->clone());
  if (expire_at != Db::kNoExpire) target_db.set_expire(destination, expire_at);
  client.out().integer(1);
}

void cmd_object(Client& client) {
  const std::string& subcommand = client.arg(1);
  RespWriter& out = client.out();

  if (str_ieq(subcommand, "help")) {
    out.array(1);
    out.simple_string("OBJECT <ENCODING | FREQ | IDLETIME | REFCOUNT> <key>");
    return;
  }
  if (client.argc() != 3) {
    reply_unknown_subcommand(client, "OBJECT", subcommand);
    return;
  }

  const Object* object = client.db().lookup_read(client.arg(2));
  if (object == nullptr) {
    out.error(err::kNoSuchKey);
    return;
  }

  if (str_ieq(subcommand, "encoding")) {
    out.bulk(object->encoding());
    return;
  }
  if (str_ieq(subcommand, "refcount")) {
    // credis does not share objects, so every value has exactly one reference.
    out.integer(1);
    return;
  }
  if (str_ieq(subcommand, "idletime")) {
    out.integer(0);
    return;
  }
  if (str_ieq(subcommand, "freq")) {
    out.error(
        "ERR An LFU maxmemory policy is not selected, access frequency not tracked. Please note "
        "that when switching between maxmemory policies at runtime LFU and LRU data will take some "
        "time to adjust.");
    return;
  }

  reply_unknown_subcommand(client, "OBJECT", subcommand);
}

// --- SCAN ---------------------------------------------------------------------

void cmd_scan(Client& client) {
  int64_t cursor_arg = 0;
  if (!string2ll(client.arg(1), &cursor_arg) || cursor_arg < 0) {
    client.out().error("ERR invalid cursor");
    return;
  }
  uint64_t cursor = static_cast<uint64_t>(cursor_arg);

  int64_t count = 10;
  std::string pattern;
  bool has_pattern = false;
  std::string type_filter;
  bool has_type_filter = false;

  for (size_t i = 2; i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "count") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &count)) return;
      if (count < 1) {
        client.out().error(err::kSyntax);
        return;
      }
      ++i;
    } else if (str_ieq(client.arg(i), "match") && i + 1 < client.argc()) {
      pattern = client.arg(i + 1);
      has_pattern = true;
      ++i;
    } else if (str_ieq(client.arg(i), "type") && i + 1 < client.argc()) {
      type_filter = to_lower(client.arg(i + 1));
      has_type_filter = true;
      ++i;
    } else {
      client.out().error(err::kSyntax);
      return;
    }
  }

  Db& database = client.db();
  std::vector<std::string> keys;
  // Collect during the scan and filter afterwards: the callback must not modify
  // the dict it is walking.
  std::vector<std::pair<std::string, ObjectType>> collected;

  // Bound the work per call so a huge, sparsely-matching keyspace still returns
  // promptly with a resumable cursor.
  int64_t max_iterations = count * 10;
  do {
    cursor = database.dict().scan(cursor, [&](const std::string& key, const Object& object) {
      collected.emplace_back(key, object.type());
    });
  } while (cursor != 0 && --max_iterations > 0 &&
           static_cast<int64_t>(collected.size()) < count);

  const bool all_keys = !has_pattern || pattern == "*";
  const int64_t now = mstime();
  for (auto& [key, type] : collected) {
    const int64_t expire_at = database.get_expire(key);
    if (expire_at != Db::kNoExpire && expire_at <= now) continue;
    if (!all_keys && !glob_match(pattern, key)) continue;
    if (has_type_filter && object_type_name(type) != type_filter) continue;
    keys.push_back(std::move(key));
  }

  RespWriter& out = client.out();
  out.array(2);
  out.bulk(ll2string(static_cast<int64_t>(cursor)));
  out.array(static_cast<int64_t>(keys.size()));
  for (const std::string& key : keys) out.bulk(key);
}

// --- expiration ---------------------------------------------------------------

namespace {

enum ExpireFlags : uint32_t {
  kExpireNone = 0,
  kExpireNx = 1u << 0,
  kExpireXx = 1u << 1,
  kExpireGt = 1u << 2,
  kExpireLt = 1u << 3,
};

// `basetime` is 0 for the *AT variants and "now" for the relative ones;
// `unit_ms` converts the argument to milliseconds.
void expire_generic(Client& client, int64_t basetime, bool seconds,
                    std::string_view command_name) {
  int64_t when = 0;
  if (!get_int64_arg(client, client.arg(2), &when)) return;

  uint32_t flags = kExpireNone;
  for (size_t i = 3; i < client.argc(); ++i) {
    const std::string& option = client.arg(i);
    if (str_ieq(option, "nx")) {
      flags |= kExpireNx;
    } else if (str_ieq(option, "xx")) {
      flags |= kExpireXx;
    } else if (str_ieq(option, "gt")) {
      flags |= kExpireGt;
    } else if (str_ieq(option, "lt")) {
      flags |= kExpireLt;
    } else {
      client.out().error(std::format("ERR Unsupported option {}", option));
      return;
    }
  }

  if ((flags & kExpireNx) && (flags & (kExpireXx | kExpireGt | kExpireLt))) {
    client.out().error(
        "ERR NX and XX, GT or LT options at the same time are not compatible");
    return;
  }
  if ((flags & kExpireGt) && (flags & kExpireLt)) {
    client.out().error("ERR GT and LT options at the same time are not compatible");
    return;
  }

  // Detect overflow from the unit conversion and from adding the base time,
  // rather than silently wrapping into a time in the past.
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if (seconds) {
    if (when > kMax / 1000 || when < kMin / 1000) {
      client.out().error(
          std::format("ERR invalid expire time in '{}' command", command_name));
      return;
    }
    when *= 1000;
  }
  if (when > kMax - basetime) {
    client.out().error(std::format("ERR invalid expire time in '{}' command", command_name));
    return;
  }
  when += basetime;

  Db& database = client.db();
  if (database.lookup_write(client.arg(1)) == nullptr) {
    client.out().integer(0);
    return;
  }

  if (flags != kExpireNone) {
    const int64_t current = database.get_expire(client.arg(1));
    // A key with no TTL is treated as expiring at infinity: GT can never beat
    // it, and LT always does.
    if (((flags & kExpireNx) && current != Db::kNoExpire) ||
        ((flags & kExpireXx) && current == Db::kNoExpire) ||
        ((flags & kExpireGt) && (current == Db::kNoExpire || when <= current)) ||
        ((flags & kExpireLt) && current != Db::kNoExpire && when >= current)) {
      client.out().integer(0);
      return;
    }
  }

  if (when <= mstime()) {
    // Already in the past: this is a delete, and it reports success.
    database.erase(client.arg(1));
  } else {
    database.set_expire(client.arg(1), when);
  }
  client.out().integer(1);
}

// Shared body of TTL/PTTL/EXPIRETIME/PEXPIRETIME.
void ttl_generic(Client& client, bool output_ms, bool absolute) {
  Db& database = client.db();
  if (database.lookup_read(client.arg(1)) == nullptr) {
    client.out().integer(-2);  // no such key
    return;
  }
  const int64_t expire_at = database.get_expire(client.arg(1));
  if (expire_at == Db::kNoExpire) {
    client.out().integer(-1);  // key exists but never expires
    return;
  }

  if (absolute) {
    client.out().integer(output_ms ? expire_at : expire_at / 1000);
    return;
  }
  const int64_t remaining = expire_at - mstime();
  // Redis rounds seconds to nearest rather than truncating.
  client.out().integer(output_ms ? remaining : (remaining + 500) / 1000);
}

}  // namespace

void cmd_expire(Client& client) { expire_generic(client, mstime(), true, "expire"); }
void cmd_pexpire(Client& client) { expire_generic(client, mstime(), false, "pexpire"); }
void cmd_expireat(Client& client) { expire_generic(client, 0, true, "expireat"); }
void cmd_pexpireat(Client& client) { expire_generic(client, 0, false, "pexpireat"); }

void cmd_ttl(Client& client) { ttl_generic(client, /*output_ms=*/false, /*absolute=*/false); }
void cmd_pttl(Client& client) { ttl_generic(client, /*output_ms=*/true, /*absolute=*/false); }
void cmd_expiretime(Client& client) {
  ttl_generic(client, /*output_ms=*/false, /*absolute=*/true);
}
void cmd_pexpiretime(Client& client) {
  ttl_generic(client, /*output_ms=*/true, /*absolute=*/true);
}

void cmd_persist(Client& client) {
  Db& database = client.db();
  if (database.lookup_write(client.arg(1)) == nullptr) {
    client.out().integer(0);
    return;
  }
  client.out().integer(database.remove_expire(client.arg(1)) ? 1 : 0);
}

}  // namespace credis
