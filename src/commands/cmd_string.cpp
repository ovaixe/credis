#include <cmath>
#include <format>
#include <limits>
#include <string>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"
#include "util/time.h"

namespace credis {
namespace {

enum SetFlags : uint32_t {
  kSetNone = 0,
  kSetNx = 1u << 0,
  kSetXx = 1u << 1,
  kSetGet = 1u << 2,
  kSetKeepTtl = 1u << 3,
  kSetPersist = 1u << 4,
};

enum class ExpireUnit { None, Seconds, Milliseconds, SecondsAt, MillisecondsAt };

// Converts an EX/PX/EXAT/PXAT argument to an absolute millisecond deadline,
// replying with Redis's error text on bad input.
bool parse_expire_value(Client& client, std::string_view value, ExpireUnit unit,
                        std::string_view command_name, int64_t* out) {
  int64_t amount = 0;
  if (!get_int64_arg(client, value, &amount)) return false;

  const bool in_seconds = unit == ExpireUnit::Seconds || unit == ExpireUnit::SecondsAt;
  if (amount <= 0 ||
      (in_seconds && amount > std::numeric_limits<int64_t>::max() / 1000)) {
    client.out().error(std::format("ERR invalid expire time in '{}' command", command_name));
    return false;
  }
  if (in_seconds) amount *= 1000;

  const bool relative = unit == ExpireUnit::Seconds || unit == ExpireUnit::Milliseconds;
  if (relative) amount += mstime();

  *out = amount;
  return true;
}

// Shared option parsing for SET and GETEX, starting at argv[start].
bool parse_set_options(Client& client, size_t start, uint32_t allowed, std::string_view name,
                       uint32_t* flags, int64_t* expire_at, bool* has_expire) {
  *flags = kSetNone;
  *has_expire = false;

  for (size_t i = start; i < client.argc(); ++i) {
    const std::string& option = client.arg(i);
    ExpireUnit unit = ExpireUnit::None;

    if (str_ieq(option, "nx") && (allowed & kSetNx)) {
      if (*flags & kSetXx) {
        client.out().error(err::kSyntax);
        return false;
      }
      *flags |= kSetNx;
      continue;
    }
    if (str_ieq(option, "xx") && (allowed & kSetXx)) {
      if (*flags & kSetNx) {
        client.out().error(err::kSyntax);
        return false;
      }
      *flags |= kSetXx;
      continue;
    }
    if (str_ieq(option, "get") && (allowed & kSetGet)) {
      *flags |= kSetGet;
      continue;
    }
    if (str_ieq(option, "keepttl") && (allowed & kSetKeepTtl)) {
      if (*has_expire) {
        client.out().error(err::kSyntax);
        return false;
      }
      *flags |= kSetKeepTtl;
      continue;
    }
    if (str_ieq(option, "persist") && (allowed & kSetPersist)) {
      if (*has_expire) {
        client.out().error(err::kSyntax);
        return false;
      }
      *flags |= kSetPersist;
      continue;
    }

    if (str_ieq(option, "ex")) unit = ExpireUnit::Seconds;
    else if (str_ieq(option, "px")) unit = ExpireUnit::Milliseconds;
    else if (str_ieq(option, "exat")) unit = ExpireUnit::SecondsAt;
    else if (str_ieq(option, "pxat")) unit = ExpireUnit::MillisecondsAt;

    if (unit == ExpireUnit::None || i + 1 >= client.argc() || *has_expire ||
        (*flags & (kSetKeepTtl | kSetPersist))) {
      client.out().error(err::kSyntax);
      return false;
    }
    if (!parse_expire_value(client, client.arg(i + 1), unit, name, expire_at)) return false;
    *has_expire = true;
    ++i;
  }
  return true;
}

}  // namespace

void cmd_set(Client& client) {
  uint32_t flags = kSetNone;
  int64_t expire_at = 0;
  bool has_expire = false;
  if (!parse_set_options(client, 3, kSetNx | kSetXx | kSetGet | kSetKeepTtl, "set", &flags,
                         &expire_at, &has_expire)) {
    return;
  }

  Db& database = client.db();
  const std::string& key = client.arg(1);
  Object* existing = database.lookup_write(key);

  // With GET, the old value is returned even when the write is skipped — but
  // only if it really is a string.
  if (flags & kSetGet) {
    if (existing != nullptr && !existing->is(ObjectType::String)) {
      reply_wrong_type(client);
      return;
    }
  }

  const bool abort = ((flags & kSetNx) && existing != nullptr) ||
                     ((flags & kSetXx) && existing == nullptr);
  if (abort) {
    if (flags & kSetGet) {
      if (existing == nullptr) {
        client.out().null_bulk();
      } else {
        client.out().bulk(existing->as_string());
      }
    } else {
      client.out().null_bulk();
    }
    return;
  }

  std::string old_value;
  const bool had_old = (flags & kSetGet) && existing != nullptr;
  if (had_old) old_value = existing->as_string();

  database.set(key, Object::make_string(client.arg(2)), (flags & kSetKeepTtl) != 0);
  if (has_expire) database.set_expire(key, expire_at);

  if (flags & kSetGet) {
    if (had_old) {
      client.out().bulk(old_value);
    } else {
      client.out().null_bulk();
    }
    return;
  }
  client.out().ok();
}

void cmd_setnx(Client& client) {
  Db& database = client.db();
  if (database.lookup_write(client.arg(1)) != nullptr) {
    client.out().integer(0);
    return;
  }
  database.set(client.arg(1), Object::make_string(client.arg(2)));
  client.out().integer(1);
}

namespace {

void setex_generic(Client& client, ExpireUnit unit, std::string_view command_name) {
  int64_t expire_at = 0;
  if (!parse_expire_value(client, client.arg(2), unit, command_name, &expire_at)) return;

  Db& database = client.db();
  database.set(client.arg(1), Object::make_string(client.arg(3)));
  database.set_expire(client.arg(1), expire_at);
  client.out().ok();
}

}  // namespace

void cmd_setex(Client& client) { setex_generic(client, ExpireUnit::Seconds, "setex"); }
void cmd_psetex(Client& client) { setex_generic(client, ExpireUnit::Milliseconds, "psetex"); }

void cmd_get(Client& client) {
  const Object* object = client.db().lookup_read(client.arg(1));
  note_lookup(client, object != nullptr);
  if (object == nullptr) {
    client.out().null_bulk();
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }
  client.out().bulk(object->as_string());
}

void cmd_getset(Client& client) {
  Db& database = client.db();
  Object* existing = database.lookup_write(client.arg(1));
  if (existing != nullptr && !existing->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }

  std::string old_value;
  const bool had_old = existing != nullptr;
  if (had_old) old_value = existing->as_string();

  // GETSET always clears any TTL.
  database.set(client.arg(1), Object::make_string(client.arg(2)));

  if (had_old) {
    client.out().bulk(old_value);
  } else {
    client.out().null_bulk();
  }
}

void cmd_getdel(Client& client) {
  Db& database = client.db();
  const Object* object = database.lookup_write(client.arg(1));
  if (object == nullptr) {
    client.out().null_bulk();
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }
  const std::string value = object->as_string();
  database.erase(client.arg(1));
  client.out().bulk(value);
}

void cmd_getex(Client& client) {
  uint32_t flags = kSetNone;
  int64_t expire_at = 0;
  bool has_expire = false;
  if (!parse_set_options(client, 2, kSetPersist, "getex", &flags, &expire_at, &has_expire)) {
    return;
  }

  Db& database = client.db();
  const Object* object = database.lookup_read(client.arg(1));
  if (object == nullptr) {
    client.out().null_bulk();
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }

  const std::string value = object->as_string();
  if (flags & kSetPersist) {
    database.remove_expire(client.arg(1));
  } else if (has_expire) {
    if (expire_at <= mstime()) {
      database.erase(client.arg(1));
    } else {
      database.set_expire(client.arg(1), expire_at);
    }
  }
  client.out().bulk(value);
}

void cmd_mset(Client& client) {
  if (client.argc() % 2 == 0) {
    reply_wrong_args(client, "mset");
    return;
  }
  Db& database = client.db();
  for (size_t i = 1; i + 1 < client.argc(); i += 2) {
    database.set(client.arg(i), Object::make_string(client.arg(i + 1)));
  }
  client.out().ok();
}

void cmd_msetnx(Client& client) {
  if (client.argc() % 2 == 0) {
    reply_wrong_args(client, "msetnx");
    return;
  }
  Db& database = client.db();
  // All or nothing: if any key already exists, nothing is written.
  for (size_t i = 1; i + 1 < client.argc(); i += 2) {
    if (database.lookup_read(client.arg(i)) != nullptr) {
      client.out().integer(0);
      return;
    }
  }
  for (size_t i = 1; i + 1 < client.argc(); i += 2) {
    database.set(client.arg(i), Object::make_string(client.arg(i + 1)));
  }
  client.out().integer(1);
}

void cmd_mget(Client& client) {
  RespWriter& out = client.out();
  out.array(static_cast<int64_t>(client.argc() - 1));
  for (size_t i = 1; i < client.argc(); ++i) {
    const Object* object = client.db().lookup_read(client.arg(i));
    // A wrong-typed key is reported as nil rather than failing the whole command.
    if (object == nullptr || !object->is(ObjectType::String)) {
      out.null_bulk();
    } else {
      out.bulk(object->as_string());
    }
  }
}

void cmd_append(Client& client) {
  Db& database = client.db();
  Object* object = database.lookup_write(client.arg(1));
  if (object == nullptr) {
    database.set(client.arg(1), Object::make_string(client.arg(2)));
    client.out().integer(static_cast<int64_t>(client.arg(2).size()));
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }
  if (object->as_string().size() + client.arg(2).size() >
      static_cast<size_t>(kMaxBulkLen)) {
    client.out().error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
    return;
  }
  object->as_string() += client.arg(2);
  client.out().integer(static_cast<int64_t>(object->as_string().size()));
}

void cmd_strlen(Client& client) {
  const Object* object = client.db().lookup_read(client.arg(1));
  if (object == nullptr) {
    client.out().integer(0);
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }
  client.out().integer(static_cast<int64_t>(object->as_string().size()));
}

void cmd_getrange(Client& client) {
  int64_t start = 0;
  int64_t end = 0;
  if (!get_int64_arg(client, client.arg(2), &start)) return;
  if (!get_int64_arg(client, client.arg(3), &end)) return;

  const Object* object = client.db().lookup_read(client.arg(1));
  if (object == nullptr) {
    client.out().bulk("");
    return;
  }
  if (!object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }

  const std::string& value = object->as_string();
  const auto length = static_cast<int64_t>(value.size());
  normalize_range(length, &start, &end);
  if (length == 0 || start > end || start >= length) {
    client.out().bulk("");
    return;
  }
  client.out().bulk(std::string_view(value).substr(static_cast<size_t>(start),
                                                   static_cast<size_t>(end - start + 1)));
}

void cmd_setrange(Client& client) {
  int64_t offset = 0;
  if (!get_int64_arg(client, client.arg(2), &offset)) return;
  if (offset < 0) {
    client.out().error("ERR offset is out of range");
    return;
  }

  const std::string& patch = client.arg(3);
  Db& database = client.db();
  Object* object = database.lookup_write(client.arg(1));

  if (object != nullptr && !object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }
  if (object == nullptr && patch.empty()) {
    // Nothing to write and no key to create.
    client.out().integer(0);
    return;
  }
  if (static_cast<size_t>(offset) + patch.size() > static_cast<size_t>(kMaxBulkLen)) {
    client.out().error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
    return;
  }

  if (object == nullptr) {
    object = database.set(client.arg(1), Object::make_string(std::string()));
  }
  std::string& value = object->as_string();
  if (patch.empty()) {
    client.out().integer(static_cast<int64_t>(value.size()));
    return;
  }
  // Gaps created by writing past the end are zero-padded.
  if (value.size() < static_cast<size_t>(offset) + patch.size()) {
    value.resize(static_cast<size_t>(offset) + patch.size(), '\0');
  }
  value.replace(static_cast<size_t>(offset), patch.size(), patch);
  client.out().integer(static_cast<int64_t>(value.size()));
}

namespace {

void incr_decr_generic(Client& client, int64_t increment) {
  Db& database = client.db();
  Object* object = database.lookup_write(client.arg(1));
  if (object != nullptr && !object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }

  int64_t value = 0;
  if (object != nullptr && !string2ll(object->as_string(), &value)) {
    client.out().error(err::kNotInteger);
    return;
  }

  // Signed overflow is undefined behaviour, so check before adding.
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if ((increment > 0 && value > kMax - increment) ||
      (increment < 0 && value < kMin - increment)) {
    client.out().error(err::kIncrOverflow);
    return;
  }
  value += increment;

  if (object != nullptr) {
    object->as_string() = ll2string(value);
  } else {
    database.set(client.arg(1), Object::make_string(ll2string(value)));
  }
  client.out().integer(value);
}

}  // namespace

void cmd_incr(Client& client) { incr_decr_generic(client, 1); }
void cmd_decr(Client& client) { incr_decr_generic(client, -1); }

void cmd_incrby(Client& client) {
  int64_t increment = 0;
  if (!get_int64_arg(client, client.arg(2), &increment)) return;
  incr_decr_generic(client, increment);
}

void cmd_decrby(Client& client) {
  int64_t decrement = 0;
  if (!get_int64_arg(client, client.arg(2), &decrement)) return;
  // -LLONG_MIN overflows, so reject it rather than wrapping.
  if (decrement == std::numeric_limits<int64_t>::min()) {
    client.out().error(err::kIncrOverflow);
    return;
  }
  incr_decr_generic(client, -decrement);
}

void cmd_incrbyfloat(Client& client) {
  // Accumulated in long double, as Redis does: at double precision
  // 10.5 + 0.1 would report 10.59999999999999964 instead of 10.6.
  long double increment = 0;
  if (!string2ld(client.arg(2), &increment)) {
    client.out().error(err::kNotFloat);
    return;
  }

  Db& database = client.db();
  Object* object = database.lookup_write(client.arg(1));
  if (object != nullptr && !object->is(ObjectType::String)) {
    reply_wrong_type(client);
    return;
  }

  long double value = 0;
  if (object != nullptr && !string2ld(object->as_string(), &value)) {
    client.out().error(err::kNotFloat);
    return;
  }

  value += increment;
  if (std::isnan(value) || std::isinf(value)) {
    client.out().error("ERR increment would produce NaN or Infinity");
    return;
  }

  const std::string formatted = ld2string_human(value);
  if (object != nullptr) {
    object->as_string() = formatted;
  } else {
    database.set(client.arg(1), Object::make_string(formatted));
  }
  // The new value is returned as a bulk string, not a double.
  client.out().bulk(formatted);
}

}  // namespace credis
