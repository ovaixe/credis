#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"

namespace credis {
namespace {

using Hash = Object::Hash;

Hash* lookup_hash(Client& client, std::string_view key, bool* ok) {
  *ok = true;
  Object* object = client.db().lookup_write(key);
  if (object == nullptr) return nullptr;
  if (!object->is(ObjectType::Hash)) {
    reply_wrong_type(client);
    *ok = false;
    return nullptr;
  }
  return &object->as_hash();
}

}  // namespace

void cmd_hset(Client& client) {
  // Field/value pairs must be complete.
  if (client.argc() % 2 != 0) {
    reply_wrong_args(client, to_lower(client.arg(0)));
    return;
  }

  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    hash = &client.db().set(client.arg(1), Object::make_hash())->as_hash();
  }

  int64_t added = 0;
  for (size_t i = 2; i + 1 < client.argc(); i += 2) {
    if (hash->set(client.arg(i), client.arg(i + 1))) ++added;
  }
  client.out().integer(added);
}

// HMSET is HSET with a status reply, kept for older clients.
void cmd_hmset(Client& client) {
  if (client.argc() % 2 != 0) {
    reply_wrong_args(client, "hmset");
    return;
  }
  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    hash = &client.db().set(client.arg(1), Object::make_hash())->as_hash();
  }
  for (size_t i = 2; i + 1 < client.argc(); i += 2) {
    hash->set(client.arg(i), client.arg(i + 1));
  }
  client.out().ok();
}

void cmd_hsetnx(Client& client) {
  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash != nullptr && hash->contains(client.arg(2))) {
    client.out().integer(0);
    return;
  }
  if (hash == nullptr) {
    hash = &client.db().set(client.arg(1), Object::make_hash())->as_hash();
  }
  hash->set(client.arg(2), client.arg(3));
  client.out().integer(1);
}

void cmd_hget(Client& client) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  const std::string* value = hash == nullptr ? nullptr : hash->find(client.arg(2));
  note_lookup(client, value != nullptr);
  if (value == nullptr) {
    client.out().null_bulk();
  } else {
    client.out().bulk(*value);
  }
}

void cmd_hmget(Client& client) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;

  client.out().array(static_cast<int64_t>(client.argc() - 2));
  for (size_t i = 2; i < client.argc(); ++i) {
    const std::string* value = hash == nullptr ? nullptr : hash->find(client.arg(i));
    if (value == nullptr) {
      client.out().null_bulk();
    } else {
      client.out().bulk(*value);
    }
  }
}

void cmd_hdel(Client& client) {
  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    client.out().integer(0);
    return;
  }

  int64_t deleted = 0;
  for (size_t i = 2; i < client.argc(); ++i) {
    if (hash->erase(client.arg(i))) ++deleted;
  }
  // A hash that loses its last field stops existing.
  if (hash->empty()) client.db().erase(client.arg(1));
  client.out().integer(deleted);
}

void cmd_hlen(Client& client) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(hash == nullptr ? 0 : static_cast<int64_t>(hash->size()));
}

void cmd_hexists(Client& client) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(hash != nullptr && hash->contains(client.arg(2)) ? 1 : 0);
}

void cmd_hstrlen(Client& client) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  const std::string* value = hash == nullptr ? nullptr : hash->find(client.arg(2));
  client.out().integer(value == nullptr ? 0 : static_cast<int64_t>(value->size()));
}

namespace {

enum class HashReply { Keys, Values, Both };

void hgetall_generic(Client& client, HashReply what) {
  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;

  if (hash == nullptr) {
    if (what == HashReply::Both) {
      client.out().map(0);
    } else {
      client.out().array(0);
    }
    return;
  }

  // The aggregate header needs the element count, so gather first.
  std::vector<std::pair<const std::string*, const std::string*>> entries;
  entries.reserve(hash->size());
  hash->for_each([&](const std::string& field, const std::string& value) {
    entries.emplace_back(&field, &value);
  });

  if (what == HashReply::Both) {
    // HGETALL is a map in RESP3 and a flat field/value array in RESP2.
    client.out().map(static_cast<int64_t>(entries.size()));
    for (const auto& [field, value] : entries) {
      client.out().bulk(*field);
      client.out().bulk(*value);
    }
    return;
  }

  client.out().array(static_cast<int64_t>(entries.size()));
  for (const auto& [field, value] : entries) {
    client.out().bulk(what == HashReply::Keys ? *field : *value);
  }
}

}  // namespace

void cmd_hkeys(Client& client) { hgetall_generic(client, HashReply::Keys); }
void cmd_hvals(Client& client) { hgetall_generic(client, HashReply::Values); }
void cmd_hgetall(Client& client) { hgetall_generic(client, HashReply::Both); }

void cmd_hincrby(Client& client) {
  int64_t increment = 0;
  if (!get_int64_arg(client, client.arg(3), &increment)) return;

  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    hash = &client.db().set(client.arg(1), Object::make_hash())->as_hash();
  }

  int64_t value = 0;
  std::string* existing = hash->find(client.arg(2));
  if (existing != nullptr && !string2ll(*existing, &value)) {
    client.out().error("ERR hash value is not an integer");
    return;
  }

  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if ((increment > 0 && value > kMax - increment) ||
      (increment < 0 && value < kMin - increment)) {
    client.out().error(err::kIncrOverflow);
    return;
  }
  value += increment;

  hash->set(client.arg(2), ll2string(value));
  client.out().integer(value);
}

void cmd_hincrbyfloat(Client& client) {
  long double increment = 0;
  if (!string2ld(client.arg(3), &increment)) {
    client.out().error(err::kNotFloat);
    return;
  }

  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    hash = &client.db().set(client.arg(1), Object::make_hash())->as_hash();
  }

  long double value = 0;
  std::string* existing = hash->find(client.arg(2));
  if (existing != nullptr && !string2ld(*existing, &value)) {
    client.out().error("ERR hash value is not a float");
    return;
  }

  value += increment;
  if (std::isnan(value) || std::isinf(value)) {
    client.out().error("ERR increment would produce NaN or Infinity");
    return;
  }

  const std::string formatted = ld2string_human(value);
  hash->set(client.arg(2), formatted);
  client.out().bulk(formatted);
}

void cmd_hrandfield(Client& client) {
  bool has_count = client.argc() >= 3;
  int64_t count = 1;
  bool with_values = false;

  if (has_count) {
    if (!get_int64_arg(client, client.arg(2), &count)) return;
    if (client.argc() == 4) {
      if (!str_ieq(client.arg(3), "withvalues")) {
        client.out().error(err::kSyntax);
        return;
      }
      with_values = true;
    } else if (client.argc() > 4) {
      client.out().error(err::kSyntax);
      return;
    }
  }

  bool ok = true;
  Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr || hash->empty()) {
    if (has_count) {
      client.out().array(0);
    } else {
      client.out().null_bulk();
    }
    return;
  }

  if (!has_count) {
    auto* entry = hash->random_entry();
    client.out().bulk(entry->key);
    return;
  }

  // A negative count allows repeats and always returns exactly |count| items;
  // a positive count returns distinct fields, capped at the hash size.
  if (count < 0) {
    const int64_t wanted = -count;
    if (with_values) {
      client.out().array(wanted * 2);
    } else {
      client.out().array(wanted);
    }
    for (int64_t i = 0; i < wanted; ++i) {
      auto* entry = hash->random_entry();
      client.out().bulk(entry->key);
      if (with_values) client.out().bulk(entry->value);
    }
    return;
  }

  std::vector<std::pair<const std::string*, const std::string*>> entries;
  entries.reserve(hash->size());
  hash->for_each([&](const std::string& field, const std::string& value) {
    entries.emplace_back(&field, &value);
  });
  const auto wanted = std::min<size_t>(static_cast<size_t>(count), entries.size());

  if (with_values) {
    client.out().array(static_cast<int64_t>(wanted) * 2);
  } else {
    client.out().array(static_cast<int64_t>(wanted));
  }
  for (size_t i = 0; i < wanted; ++i) {
    client.out().bulk(*entries[i].first);
    if (with_values) client.out().bulk(*entries[i].second);
  }
}

void cmd_hscan(Client& client) {
  ScanOptions options;
  if (!parse_scan_options(client, 3, /*allow_novalues=*/true, &options)) return;

  bool ok = true;
  const Hash* hash = lookup_hash(client, client.arg(1), &ok);
  if (!ok) return;
  if (hash == nullptr) {
    client.out().array(2);
    client.out().bulk("0");
    client.out().array(0);
    return;
  }

  // Collect during the walk, filter after: the callback must not touch the dict
  // it is iterating.
  std::vector<std::pair<std::string, std::string>> collected;
  uint64_t cursor = options.cursor;
  int64_t max_iterations = options.count * 10;
  do {
    cursor = hash->scan(cursor, [&](const std::string& field, const std::string& value) {
      collected.emplace_back(field, value);
    });
  } while (cursor != 0 && --max_iterations > 0 &&
           static_cast<int64_t>(collected.size()) < options.count);

  std::vector<std::pair<std::string, std::string>> matched;
  for (auto& entry : collected) {
    if (scan_matches(options, entry.first)) matched.push_back(std::move(entry));
  }

  client.out().array(2);
  client.out().bulk(ll2string(static_cast<int64_t>(cursor)));
  // NOVALUES returns just the field names.
  client.out().array(static_cast<int64_t>(matched.size()) * (options.novalues ? 1 : 2));
  for (const auto& [field, value] : matched) {
    client.out().bulk(field);
    if (!options.novalues) client.out().bulk(value);
  }
}

}  // namespace credis
