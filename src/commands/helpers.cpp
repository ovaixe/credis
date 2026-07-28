#include "commands/commands.h"

#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "util/strings.h"

namespace credis {

bool get_int64_arg(Client& client, std::string_view value, int64_t* out,
                   std::string_view error) {
  if (!string2ll(value, out)) {
    client.out().error(error);
    return false;
  }
  return true;
}

bool get_double_arg(Client& client, std::string_view value, double* out,
                    std::string_view error) {
  if (!string2d(value, out)) {
    client.out().error(error);
    return false;
  }
  return true;
}

bool check_type(Client& client, const Object* object, ObjectType expected) {
  if (object == nullptr || object->is(expected)) return true;
  reply_wrong_type(client);
  return false;
}

void note_lookup(Client& client, bool found) {
  if (found) {
    ++client.server().stats().keyspace_hits;
  } else {
    ++client.server().stats().keyspace_misses;
  }
}

void delete_if_empty(Client& client, std::string_view key, const Object* object) {
  if (object != nullptr && object->is_empty_container()) {
    client.db().erase(key);
  }
}

bool parse_scan_options(Client& client, size_t first_option, bool allow_novalues,
                        ScanOptions* out) {
  int64_t cursor = 0;
  if (!string2ll(client.arg(first_option - 1), &cursor) || cursor < 0) {
    client.out().error("ERR invalid cursor");
    return false;
  }
  out->cursor = static_cast<uint64_t>(cursor);

  for (size_t i = first_option; i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "count") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &out->count)) return false;
      if (out->count < 1) {
        client.out().error(err::kSyntax);
        return false;
      }
      ++i;
    } else if (str_ieq(client.arg(i), "match") && i + 1 < client.argc()) {
      out->pattern = client.arg(i + 1);
      out->has_pattern = true;
      ++i;
    } else if (allow_novalues && str_ieq(client.arg(i), "novalues")) {
      out->novalues = true;
    } else {
      client.out().error(err::kSyntax);
      return false;
    }
  }
  return true;
}

bool scan_matches(const ScanOptions& options, std::string_view key) {
  if (!options.has_pattern || options.pattern == "*") return true;
  return glob_match(options.pattern, key);
}

void normalize_range(int64_t length, int64_t* start, int64_t* end) {
  if (*start < 0) *start = length + *start;
  if (*end < 0) *end = length + *end;
  if (*start < 0) *start = 0;
  if (*end >= length) *end = length - 1;
}

}  // namespace credis
