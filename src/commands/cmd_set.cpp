#include <algorithm>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"

namespace credis {
namespace {

using Set = Object::Set;

Set* lookup_set(Client& client, std::string_view key, bool* ok) {
  *ok = true;
  Object* object = client.db().lookup_write(key);
  if (object == nullptr) return nullptr;
  if (!object->is(ObjectType::Set)) {
    reply_wrong_type(client);
    *ok = false;
    return nullptr;
  }
  return &object->as_set();
}

// Snapshots a set's members. Used by the algebra commands, which must read
// several sets while writing a result that may rehash the keyspace.
std::vector<std::string> members_of(const Set& set) {
  std::vector<std::string> members;
  members.reserve(set.size());
  set.for_each([&](const std::string& member, const Empty&) { members.push_back(member); });
  return members;
}

enum class SetOp { Union, Intersect, Difference };

// Reads the source sets named by argv[first_key..first_key+count) and computes
// `op` over them. Returns false if a source key held the wrong type.
bool compute_set_op(Client& client, SetOp op, size_t first_key, size_t count,
                    std::vector<std::string>* result, int64_t limit) {
  std::vector<std::vector<std::string>> sources;
  sources.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    Object* object = client.db().lookup_read(client.arg(first_key + i));
    if (object == nullptr) {
      sources.emplace_back();
      continue;
    }
    if (!object->is(ObjectType::Set)) {
      reply_wrong_type(client);
      return false;
    }
    sources.push_back(members_of(object->as_set()));
  }

  // Membership tests run against dicts rather than the vectors, keeping the
  // whole operation linear in the total number of members.
  auto as_dict = [](const std::vector<std::string>& members) {
    Dict<Empty> dict;
    for (const std::string& member : members) dict.insert(member);
    return dict;
  };

  if (op == SetOp::Union) {
    Dict<Empty> seen;
    for (const auto& source : sources) {
      for (const std::string& member : source) {
        if (seen.insert(member) != nullptr) result->push_back(member);
      }
    }
    return true;
  }

  if (sources.empty() || sources[0].empty()) return true;

  if (op == SetOp::Intersect) {
    // Starting from the smallest source keeps the candidate list short.
    size_t smallest = 0;
    for (size_t i = 1; i < sources.size(); ++i) {
      if (sources[i].size() < sources[smallest].size()) smallest = i;
    }
    std::vector<Dict<Empty>> others;
    for (size_t i = 0; i < sources.size(); ++i) {
      if (i != smallest) others.push_back(as_dict(sources[i]));
    }
    for (const std::string& member : sources[smallest]) {
      bool in_all = true;
      for (const auto& other : others) {
        if (!other.contains(member)) {
          in_all = false;
          break;
        }
      }
      if (!in_all) continue;
      result->push_back(member);
      if (limit > 0 && static_cast<int64_t>(result->size()) >= limit) break;
    }
    return true;
  }

  // Difference: everything in the first set that is in none of the others.
  std::vector<Dict<Empty>> others;
  for (size_t i = 1; i < sources.size(); ++i) others.push_back(as_dict(sources[i]));
  for (const std::string& member : sources[0]) {
    bool excluded = false;
    for (const auto& other : others) {
      if (other.contains(member)) {
        excluded = true;
        break;
      }
    }
    if (!excluded) result->push_back(member);
  }
  return true;
}

void set_op_command(Client& client, SetOp op, bool store) {
  const size_t first_key = store ? 2 : 1;
  const size_t count = client.argc() - first_key;
  if (count == 0) {
    reply_wrong_args(client, to_lower(client.arg(0)));
    return;
  }

  std::vector<std::string> result;
  if (!compute_set_op(client, op, first_key, count, &result, 0)) return;

  if (!store) {
    client.out().set(static_cast<int64_t>(result.size()));
    for (const std::string& member : result) client.out().bulk(member);
    return;
  }

  const std::string& destination = client.arg(1);
  if (result.empty()) {
    // An empty result deletes the destination rather than storing an empty set.
    client.db().erase(destination);
    client.out().integer(0);
    return;
  }
  Object stored = Object::make_set();
  for (const std::string& member : result) stored.as_set().insert(member);
  client.db().set(destination, std::move(stored));
  client.out().integer(static_cast<int64_t>(result.size()));
}

}  // namespace

void cmd_sadd(Client& client) {
  bool ok = true;
  Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr) {
    set = &client.db().set(client.arg(1), Object::make_set())->as_set();
  }

  int64_t added = 0;
  for (size_t i = 2; i < client.argc(); ++i) {
    if (set->insert(client.arg(i)) != nullptr) ++added;
  }
  client.out().integer(added);
}

void cmd_srem(Client& client) {
  bool ok = true;
  Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr) {
    client.out().integer(0);
    return;
  }

  int64_t removed = 0;
  for (size_t i = 2; i < client.argc(); ++i) {
    if (set->erase(client.arg(i))) ++removed;
  }
  if (set->empty()) client.db().erase(client.arg(1));
  client.out().integer(removed);
}

void cmd_scard(Client& client) {
  bool ok = true;
  const Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(set == nullptr ? 0 : static_cast<int64_t>(set->size()));
}

void cmd_sismember(Client& client) {
  bool ok = true;
  const Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(set != nullptr && set->contains(client.arg(2)) ? 1 : 0);
}

void cmd_smismember(Client& client) {
  bool ok = true;
  const Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;

  client.out().array(static_cast<int64_t>(client.argc() - 2));
  for (size_t i = 2; i < client.argc(); ++i) {
    client.out().integer(set != nullptr && set->contains(client.arg(i)) ? 1 : 0);
  }
}

void cmd_smembers(Client& client) {
  bool ok = true;
  const Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr) {
    client.out().set(0);
    return;
  }

  const std::vector<std::string> members = members_of(*set);
  client.out().set(static_cast<int64_t>(members.size()));
  for (const std::string& member : members) client.out().bulk(member);
}

void cmd_spop(Client& client) {
  const bool has_count = client.argc() == 3;
  int64_t count = 1;
  if (has_count) {
    if (!get_int64_arg(client, client.arg(2), &count)) return;
    if (count < 0) {
      client.out().error(err::kOutOfRange);
      return;
    }
  } else if (client.argc() > 3) {
    client.out().error(err::kSyntax);
    return;
  }

  bool ok = true;
  Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr) {
    if (has_count) {
      client.out().set(0);
    } else {
      client.out().null_bulk();
    }
    return;
  }

  if (!has_count) {
    auto* entry = set->random_entry();
    const std::string member = entry->key;
    set->erase(member);
    if (set->empty()) client.db().erase(client.arg(1));
    client.out().bulk(member);
    return;
  }

  const auto wanted = std::min<size_t>(static_cast<size_t>(count), set->size());
  std::vector<std::string> popped;
  popped.reserve(wanted);
  for (size_t i = 0; i < wanted; ++i) {
    auto* entry = set->random_entry();
    if (entry == nullptr) break;
    popped.push_back(entry->key);
    set->erase(popped.back());
  }
  if (set->empty()) client.db().erase(client.arg(1));

  client.out().set(static_cast<int64_t>(popped.size()));
  for (const std::string& member : popped) client.out().bulk(member);
}

void cmd_srandmember(Client& client) {
  const bool has_count = client.argc() == 3;
  int64_t count = 1;
  if (has_count) {
    if (!get_int64_arg(client, client.arg(2), &count)) return;
  } else if (client.argc() > 3) {
    client.out().error(err::kSyntax);
    return;
  }

  bool ok = true;
  Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr || set->empty()) {
    if (has_count) {
      client.out().array(0);
    } else {
      client.out().null_bulk();
    }
    return;
  }

  if (!has_count) {
    client.out().bulk(set->random_entry()->key);
    return;
  }

  // Negative count: allow repeats, return exactly |count|.
  if (count < 0) {
    const int64_t wanted = -count;
    client.out().array(wanted);
    for (int64_t i = 0; i < wanted; ++i) client.out().bulk(set->random_entry()->key);
    return;
  }

  const std::vector<std::string> members = members_of(*set);
  const auto wanted = std::min<size_t>(static_cast<size_t>(count), members.size());
  client.out().array(static_cast<int64_t>(wanted));
  for (size_t i = 0; i < wanted; ++i) client.out().bulk(members[i]);
}

void cmd_smove(Client& client) {
  Db& database = client.db();
  const std::string& source = client.arg(1);
  const std::string& destination = client.arg(2);
  const std::string& member = client.arg(3);

  Object* source_object = database.lookup_write(source);
  if (source_object == nullptr) {
    client.out().integer(0);
    return;
  }
  if (!source_object->is(ObjectType::Set)) {
    reply_wrong_type(client);
    return;
  }
  Object* destination_object = database.lookup_write(destination);
  if (destination_object != nullptr && !destination_object->is(ObjectType::Set)) {
    reply_wrong_type(client);
    return;
  }
  if (!source_object->as_set().contains(member)) {
    client.out().integer(0);
    return;
  }
  if (source == destination) {
    // Moving within one set is a no-op that still reports success.
    client.out().integer(1);
    return;
  }

  source_object->as_set().erase(member);
  if (source_object->as_set().empty()) database.erase(source);

  // Re-resolve: erasing the source may have rehashed the keyspace.
  destination_object = database.lookup_write(destination);
  if (destination_object == nullptr) {
    destination_object = database.set(destination, Object::make_set());
  }
  destination_object->as_set().insert(member);
  client.out().integer(1);
}

void cmd_sunion(Client& client) { set_op_command(client, SetOp::Union, false); }
void cmd_sinter(Client& client) { set_op_command(client, SetOp::Intersect, false); }
void cmd_sdiff(Client& client) { set_op_command(client, SetOp::Difference, false); }
void cmd_sunionstore(Client& client) { set_op_command(client, SetOp::Union, true); }
void cmd_sinterstore(Client& client) { set_op_command(client, SetOp::Intersect, true); }
void cmd_sdiffstore(Client& client) { set_op_command(client, SetOp::Difference, true); }

void cmd_sintercard(Client& client) {
  int64_t numkeys = 0;
  if (!get_int64_arg(client, client.arg(1), &numkeys, "ERR numkeys should be greater than 0")) {
    return;
  }
  if (numkeys <= 0 || static_cast<size_t>(numkeys) + 2 > client.argc()) {
    client.out().error("ERR numkeys should be greater than 0");
    return;
  }

  int64_t limit = 0;
  const size_t after_keys = static_cast<size_t>(numkeys) + 2;
  if (after_keys < client.argc()) {
    if (!str_ieq(client.arg(after_keys), "limit") || after_keys + 1 >= client.argc()) {
      client.out().error(err::kSyntax);
      return;
    }
    if (!get_int64_arg(client, client.arg(after_keys + 1), &limit)) return;
    if (limit < 0) {
      client.out().error("ERR LIMIT can't be negative");
      return;
    }
  }

  std::vector<std::string> result;
  if (!compute_set_op(client, SetOp::Intersect, 2, static_cast<size_t>(numkeys), &result, limit)) {
    return;
  }
  client.out().integer(static_cast<int64_t>(result.size()));
}

void cmd_sscan(Client& client) {
  ScanOptions options;
  if (!parse_scan_options(client, 3, /*allow_novalues=*/false, &options)) return;

  bool ok = true;
  const Set* set = lookup_set(client, client.arg(1), &ok);
  if (!ok) return;
  if (set == nullptr) {
    client.out().array(2);
    client.out().bulk("0");
    client.out().array(0);
    return;
  }

  std::vector<std::string> collected;
  uint64_t cursor = options.cursor;
  int64_t max_iterations = options.count * 10;
  do {
    cursor = set->scan(cursor, [&](const std::string& member, const Empty&) {
      collected.push_back(member);
    });
  } while (cursor != 0 && --max_iterations > 0 &&
           static_cast<int64_t>(collected.size()) < options.count);

  std::vector<std::string> matched;
  for (std::string& member : collected) {
    if (scan_matches(options, member)) matched.push_back(std::move(member));
  }

  client.out().array(2);
  client.out().bulk(ll2string(static_cast<int64_t>(cursor)));
  client.out().array(static_cast<int64_t>(matched.size()));
  for (const std::string& member : matched) client.out().bulk(member);
}

}  // namespace credis
