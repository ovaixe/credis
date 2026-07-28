#include <algorithm>
#include <deque>
#include <format>
#include <string>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"

namespace credis {
namespace {

using List = Object::List;

// Fetches the list at `key`, replying WRONGTYPE if the key holds something else.
// `*ok` is false only when an error has already been sent.
List* lookup_list(Client& client, std::string_view key, bool* ok) {
  *ok = true;
  Object* object = client.db().lookup_write(key);
  if (object == nullptr) return nullptr;
  if (!object->is(ObjectType::List)) {
    reply_wrong_type(client);
    *ok = false;
    return nullptr;
  }
  return &object->as_list();
}

void push_generic(Client& client, bool left, bool require_existing) {
  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;

  if (list == nullptr) {
    if (require_existing) {
      // LPUSHX/RPUSHX never create the key.
      client.out().integer(0);
      return;
    }
    list = &client.db().set(client.arg(1), Object::make_list())->as_list();
  }

  for (size_t i = 2; i < client.argc(); ++i) {
    if (left) {
      list->push_front(client.arg(i));
    } else {
      list->push_back(client.arg(i));
    }
  }
  client.out().integer(static_cast<int64_t>(list->size()));
}

void pop_generic(Client& client, bool left) {
  if (client.argc() > 3) {
    reply_wrong_args(client, left ? "lpop" : "rpop");
    return;
  }

  const bool has_count = client.argc() == 3;
  int64_t count = 1;
  if (has_count) {
    if (!get_int64_arg(client, client.arg(2), &count)) return;
    if (count < 0) {
      client.out().error(err::kOutOfRange);
      return;
    }
  }

  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    // The count form replies with a null array rather than a null bulk.
    if (has_count) {
      client.out().null_array();
    } else {
      client.out().null_bulk();
    }
    return;
  }
  if (has_count && count == 0) {
    client.out().array(0);
    return;
  }

  const auto popped = std::min<size_t>(static_cast<size_t>(count), list->size());
  if (has_count) client.out().array(static_cast<int64_t>(popped));
  for (size_t i = 0; i < popped; ++i) {
    if (left) {
      client.out().bulk(list->front());
      list->pop_front();
    } else {
      client.out().bulk(list->back());
      list->pop_back();
    }
  }

  if (list->empty()) client.db().erase(client.arg(1));
}

}  // namespace

void cmd_lpush(Client& client) { push_generic(client, true, false); }
void cmd_rpush(Client& client) { push_generic(client, false, false); }
void cmd_lpushx(Client& client) { push_generic(client, true, true); }
void cmd_rpushx(Client& client) { push_generic(client, false, true); }
void cmd_lpop(Client& client) { pop_generic(client, true); }
void cmd_rpop(Client& client) { pop_generic(client, false); }

void cmd_llen(Client& client) {
  bool ok = true;
  const List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(list == nullptr ? 0 : static_cast<int64_t>(list->size()));
}

void cmd_lrange(Client& client) {
  int64_t start = 0;
  int64_t end = 0;
  if (!get_int64_arg(client, client.arg(2), &start)) return;
  if (!get_int64_arg(client, client.arg(3), &end)) return;

  bool ok = true;
  const List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().array(0);
    return;
  }

  const auto length = static_cast<int64_t>(list->size());
  normalize_range(length, &start, &end);
  if (start > end || start >= length) {
    client.out().array(0);
    return;
  }

  client.out().array(end - start + 1);
  for (int64_t i = start; i <= end; ++i) {
    client.out().bulk((*list)[static_cast<size_t>(i)]);
  }
}

void cmd_lindex(Client& client) {
  int64_t index = 0;
  if (!get_int64_arg(client, client.arg(2), &index)) return;

  bool ok = true;
  const List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().null_bulk();
    return;
  }

  const auto length = static_cast<int64_t>(list->size());
  if (index < 0) index += length;
  if (index < 0 || index >= length) {
    client.out().null_bulk();
    return;
  }
  client.out().bulk((*list)[static_cast<size_t>(index)]);
}

void cmd_lset(Client& client) {
  int64_t index = 0;
  if (!get_int64_arg(client, client.arg(2), &index)) return;

  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().error(err::kNoSuchKey);
    return;
  }

  const auto length = static_cast<int64_t>(list->size());
  if (index < 0) index += length;
  if (index < 0 || index >= length) {
    client.out().error(err::kIndexOutOfRange);
    return;
  }
  (*list)[static_cast<size_t>(index)] = client.arg(3);
  client.out().ok();
}

void cmd_lrem(Client& client) {
  int64_t count = 0;
  if (!get_int64_arg(client, client.arg(2), &count)) return;

  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().integer(0);
    return;
  }

  const std::string& target = client.arg(3);
  int64_t removed = 0;
  // count > 0 removes from the head, count < 0 from the tail, 0 removes all.
  const int64_t limit = count == 0 ? static_cast<int64_t>(list->size())
                                   : (count < 0 ? -count : count);

  if (count >= 0) {
    for (auto it = list->begin(); it != list->end() && removed < limit;) {
      if (*it == target) {
        it = list->erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  } else {
    for (auto it = list->end(); it != list->begin() && removed < limit;) {
      --it;
      if (*it == target) {
        it = list->erase(it);
        ++removed;
      }
    }
  }

  if (list->empty()) client.db().erase(client.arg(1));
  client.out().integer(removed);
}

void cmd_ltrim(Client& client) {
  int64_t start = 0;
  int64_t end = 0;
  if (!get_int64_arg(client, client.arg(2), &start)) return;
  if (!get_int64_arg(client, client.arg(3), &end)) return;

  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().ok();
    return;
  }

  const auto length = static_cast<int64_t>(list->size());
  normalize_range(length, &start, &end);

  if (start > end || start >= length) {
    // Trimming to an empty range deletes the key.
    client.db().erase(client.arg(1));
    client.out().ok();
    return;
  }

  list->erase(list->begin() + static_cast<long>(end) + 1, list->end());
  list->erase(list->begin(), list->begin() + static_cast<long>(start));
  if (list->empty()) client.db().erase(client.arg(1));
  client.out().ok();
}

void cmd_linsert(Client& client) {
  bool before = false;
  if (str_ieq(client.arg(2), "before")) {
    before = true;
  } else if (!str_ieq(client.arg(2), "after")) {
    client.out().error(err::kSyntax);
    return;
  }

  bool ok = true;
  List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    client.out().integer(0);
    return;
  }

  const std::string& pivot = client.arg(3);
  const auto it = std::find(list->begin(), list->end(), pivot);
  if (it == list->end()) {
    // Pivot absent: -1 distinguishes this from a missing key.
    client.out().integer(-1);
    return;
  }
  list->insert(before ? it : it + 1, client.arg(4));
  client.out().integer(static_cast<int64_t>(list->size()));
}

void cmd_lpos(Client& client) {
  int64_t rank = 1;
  int64_t count = -1;  // -1 means "no COUNT given": return a single position
  int64_t maxlen = 0;

  for (size_t i = 3; i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "rank") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &rank)) return;
      if (rank == 0) {
        client.out().error(
            "ERR RANK can't be zero. Use 1 to start searching from the first matching element "
            "in the head of the list or -1 in the tail.");
        return;
      }
      ++i;
    } else if (str_ieq(client.arg(i), "count") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &count)) return;
      if (count < 0) {
        client.out().error("ERR COUNT can't be negative");
        return;
      }
      ++i;
    } else if (str_ieq(client.arg(i), "maxlen") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &maxlen)) return;
      if (maxlen < 0) {
        client.out().error("ERR MAXLEN can't be negative");
        return;
      }
      ++i;
    } else {
      client.out().error(err::kSyntax);
      return;
    }
  }

  bool ok = true;
  const List* list = lookup_list(client, client.arg(1), &ok);
  if (!ok) return;
  if (list == nullptr) {
    if (count >= 0) {
      client.out().array(0);
    } else {
      client.out().null_bulk();
    }
    return;
  }

  const std::string& target = client.arg(2);
  const auto length = static_cast<int64_t>(list->size());
  const bool from_tail = rank < 0;
  int64_t to_skip = (from_tail ? -rank : rank) - 1;

  std::vector<int64_t> found;
  int64_t compared = 0;
  for (int64_t step = 0; step < length; ++step) {
    const int64_t index = from_tail ? length - 1 - step : step;
    ++compared;
    if (maxlen != 0 && compared > maxlen) break;
    if ((*list)[static_cast<size_t>(index)] != target) continue;
    if (to_skip > 0) {
      --to_skip;
      continue;
    }
    found.push_back(index);
    if (count == 0) continue;              // COUNT 0 means "all matches"
    if (count > 0 && static_cast<int64_t>(found.size()) >= count) break;
    if (count < 0) break;                  // no COUNT: first match only
  }

  if (count < 0) {
    if (found.empty()) {
      client.out().null_bulk();
    } else {
      client.out().integer(found[0]);
    }
    return;
  }
  client.out().array(static_cast<int64_t>(found.size()));
  for (int64_t index : found) client.out().integer(index);
}

namespace {

// Shared by RPOPLPUSH and LMOVE.
void move_generic(Client& client, const std::string& source, const std::string& destination,
                  bool from_left, bool to_left) {
  Db& database = client.db();

  Object* source_object = database.lookup_write(source);
  if (source_object == nullptr) {
    client.out().null_bulk();
    return;
  }
  if (!source_object->is(ObjectType::List)) {
    reply_wrong_type(client);
    return;
  }
  Object* destination_object = database.lookup_write(destination);
  if (destination_object != nullptr && !destination_object->is(ObjectType::List)) {
    reply_wrong_type(client);
    return;
  }

  List& source_list = source_object->as_list();
  if (source_list.empty()) {
    client.out().null_bulk();
    return;
  }

  std::string value = from_left ? source_list.front() : source_list.back();
  if (from_left) {
    source_list.pop_front();
  } else {
    source_list.pop_back();
  }

  if (destination_object == nullptr) {
    // Creating the destination can rehash the keyspace, which would invalidate
    // source_list; everything read from the source is already copied out.
    destination_object = database.set(destination, Object::make_list());
  }
  List& destination_list = destination_object->as_list();
  if (to_left) {
    destination_list.push_front(value);
  } else {
    destination_list.push_back(value);
  }

  // Re-look up the source: the insert above may have moved it.
  Object* refreshed = database.dict().find(source);
  if (refreshed != nullptr && refreshed->is(ObjectType::List) && refreshed->as_list().empty()) {
    database.erase(source);
  }
  client.out().bulk(value);
}

}  // namespace

void cmd_rpoplpush(Client& client) {
  move_generic(client, client.arg(1), client.arg(2), /*from_left=*/false, /*to_left=*/true);
}

void cmd_lmove(Client& client) {
  bool from_left = false;
  bool to_left = false;
  if (str_ieq(client.arg(3), "left")) from_left = true;
  else if (!str_ieq(client.arg(3), "right")) {
    client.out().error(err::kSyntax);
    return;
  }
  if (str_ieq(client.arg(4), "left")) to_left = true;
  else if (!str_ieq(client.arg(4), "right")) {
    client.out().error(err::kSyntax);
    return;
  }
  move_generic(client, client.arg(1), client.arg(2), from_left, to_left);
}

void cmd_lmpop(Client& client) {
  int64_t numkeys = 0;
  if (!get_int64_arg(client, client.arg(1), &numkeys)) return;
  if (numkeys <= 0 || static_cast<size_t>(numkeys) + 2 > client.argc()) {
    client.out().error("ERR numkeys should be greater than 0");
    return;
  }

  const size_t where_index = static_cast<size_t>(numkeys) + 2;
  if (where_index >= client.argc()) {
    client.out().error(err::kSyntax);
    return;
  }
  bool left = false;
  if (str_ieq(client.arg(where_index), "left")) left = true;
  else if (!str_ieq(client.arg(where_index), "right")) {
    client.out().error(err::kSyntax);
    return;
  }

  int64_t count = 1;
  if (where_index + 1 < client.argc()) {
    if (!str_ieq(client.arg(where_index + 1), "count") || where_index + 2 >= client.argc()) {
      client.out().error(err::kSyntax);
      return;
    }
    if (!get_int64_arg(client, client.arg(where_index + 2), &count)) return;
    if (count <= 0) {
      client.out().error("ERR count should be greater than 0");
      return;
    }
  }

  // Serves the first key that actually holds elements.
  for (int64_t i = 0; i < numkeys; ++i) {
    const std::string& key = client.arg(static_cast<size_t>(i) + 2);
    Object* object = client.db().lookup_write(key);
    if (object == nullptr) continue;
    if (!object->is(ObjectType::List)) {
      reply_wrong_type(client);
      return;
    }
    List& list = object->as_list();
    if (list.empty()) continue;

    const auto popped = std::min<size_t>(static_cast<size_t>(count), list.size());
    client.out().array(2);
    client.out().bulk(key);
    client.out().array(static_cast<int64_t>(popped));
    for (size_t n = 0; n < popped; ++n) {
      if (left) {
        client.out().bulk(list.front());
        list.pop_front();
      } else {
        client.out().bulk(list.back());
        list.pop_back();
      }
    }
    if (list.empty()) client.db().erase(key);
    return;
  }
  client.out().null_array();
}

}  // namespace credis
