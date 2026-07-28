#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "net/connection.h"
#include "server.h"
#include "store/db.h"
#include "store/skiplist.h"
#include "util/strings.h"

namespace credis {
namespace {

ZSet* lookup_zset(Client& client, std::string_view key, bool* ok) {
  *ok = true;
  Object* object = client.db().lookup_write(key);
  if (object == nullptr) return nullptr;
  if (!object->is(ObjectType::ZSet)) {
    reply_wrong_type(client);
    *ok = false;
    return nullptr;
  }
  return &object->as_zset();
}

// One element of a range reply, snapshotted so the reply can be written after
// any keyspace mutation.
struct ScoredMember {
  std::string member;
  double score = 0;
};

void reply_scored(Client& client, const std::vector<ScoredMember>& items, bool with_scores) {
  RespWriter& out = client.out();
  if (!with_scores) {
    out.array(static_cast<int64_t>(items.size()));
    for (const ScoredMember& item : items) out.bulk(item.member);
    return;
  }
  if (client.protocol() == RespProtocol::Resp3) {
    // RESP3 pairs each member with its score in a nested two-element array.
    out.array(static_cast<int64_t>(items.size()));
    for (const ScoredMember& item : items) {
      out.array(2);
      out.bulk(item.member);
      out.double_value(item.score);
    }
    return;
  }
  out.array(static_cast<int64_t>(items.size()) * 2);
  for (const ScoredMember& item : items) {
    out.bulk(item.member);
    out.bulk(d2string(item.score));
  }
}

enum ZAddFlags : uint32_t {
  kZAddNone = 0,
  kZAddNx = 1u << 0,
  kZAddXx = 1u << 1,
  kZAddGt = 1u << 2,
  kZAddLt = 1u << 3,
  kZAddCh = 1u << 4,
  kZAddIncr = 1u << 5,
};

}  // namespace

void cmd_zadd(Client& client) {
  uint32_t flags = kZAddNone;
  size_t i = 2;
  for (; i < client.argc(); ++i) {
    const std::string& option = client.arg(i);
    if (str_ieq(option, "nx")) flags |= kZAddNx;
    else if (str_ieq(option, "xx")) flags |= kZAddXx;
    else if (str_ieq(option, "gt")) flags |= kZAddGt;
    else if (str_ieq(option, "lt")) flags |= kZAddLt;
    else if (str_ieq(option, "ch")) flags |= kZAddCh;
    else if (str_ieq(option, "incr")) flags |= kZAddIncr;
    else break;
  }

  if ((flags & kZAddNx) && (flags & (kZAddXx | kZAddGt | kZAddLt))) {
    client.out().error("ERR GT, LT, and/or NX options at the same time are not compatible");
    return;
  }
  if ((flags & kZAddGt) && (flags & kZAddLt)) {
    client.out().error("ERR GT, LT, and/or NX options at the same time are not compatible");
    return;
  }

  // Flags must all precede the score/member pairs; the first non-flag argument
  // starts the pairs. A leftover odd argument means the caller mixed them, which
  // Redis reports as an arity error rather than a syntax error.
  const size_t remaining = client.argc() - i;
  if (remaining == 0 || remaining % 2 != 0) {
    reply_wrong_args(client, "zadd");
    return;
  }
  const size_t pairs = remaining / 2;
  if ((flags & kZAddIncr) && pairs > 1) {
    client.out().error("ERR INCR option supports a single increment-element pair");
    return;
  }

  // Parse every score up front so a bad one leaves the set untouched.
  std::vector<double> scores(pairs);
  for (size_t p = 0; p < pairs; ++p) {
    if (!get_double_arg(client, client.arg(i + p * 2), &scores[p], err::kNotFloat)) return;
  }

  bool ok = true;
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    if (flags & kZAddXx) {
      // XX never creates the key.
      if (flags & kZAddIncr) {
        client.out().null_bulk();
      } else {
        client.out().integer(0);
      }
      return;
    }
    zset = &client.db().set(client.arg(1), Object::make_zset())->as_zset();
  }

  int64_t added = 0;
  int64_t changed = 0;
  for (size_t p = 0; p < pairs; ++p) {
    const std::string& member = client.arg(i + p * 2 + 1);
    double score = scores[p];
    const double* existing = zset->score_of(member);

    if (existing == nullptr && (flags & kZAddXx)) continue;
    if (existing != nullptr && (flags & kZAddNx)) {
      if (flags & kZAddIncr) {
        // NX on an existing member: no update, and INCR reports nil.
        client.out().null_bulk();
        return;
      }
      continue;
    }

    if (flags & kZAddIncr) {
      if (existing != nullptr) score += *existing;
      if (std::isnan(score)) {
        client.out().error(err::kNanResult);
        return;
      }
    }

    if (existing != nullptr) {
      // GT/LT only move the score in the requested direction.
      if ((flags & kZAddGt) && score <= *existing) {
        if (flags & kZAddIncr) {
          client.out().null_bulk();
          return;
        }
        continue;
      }
      if ((flags & kZAddLt) && score >= *existing) {
        if (flags & kZAddIncr) {
          client.out().null_bulk();
          return;
        }
        continue;
      }
      if (*existing != score) ++changed;
    }

    if (zset->add(member, score)) ++added;

    if (flags & kZAddIncr) {
      client.out().double_value(score);
      return;
    }
  }

  if (flags & kZAddIncr) {
    // Every pair was filtered out by NX/XX/GT/LT.
    client.out().null_bulk();
    return;
  }
  client.out().integer((flags & kZAddCh) ? added + changed : added);
}

void cmd_zincrby(Client& client) {
  double increment = 0;
  if (!get_double_arg(client, client.arg(2), &increment, err::kNotFloat)) return;

  bool ok = true;
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    zset = &client.db().set(client.arg(1), Object::make_zset())->as_zset();
  }

  const std::string& member = client.arg(3);
  const double* existing = zset->score_of(member);
  const double score = existing == nullptr ? increment : *existing + increment;
  if (std::isnan(score)) {
    client.out().error(err::kNanResult);
    return;
  }
  zset->add(member, score);
  client.out().double_value(score);
}

void cmd_zscore(Client& client) {
  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  const double* score = zset == nullptr ? nullptr : zset->score_of(client.arg(2));
  note_lookup(client, score != nullptr);
  if (score == nullptr) {
    client.out().null_bulk();
  } else {
    client.out().double_value(*score);
  }
}

void cmd_zmscore(Client& client) {
  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;

  client.out().array(static_cast<int64_t>(client.argc() - 2));
  for (size_t i = 2; i < client.argc(); ++i) {
    const double* score = zset == nullptr ? nullptr : zset->score_of(client.arg(i));
    if (score == nullptr) {
      client.out().null_bulk();
    } else {
      client.out().double_value(*score);
    }
  }
}

void cmd_zcard(Client& client) {
  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  client.out().integer(zset == nullptr ? 0 : static_cast<int64_t>(zset->size()));
}

void cmd_zrem(Client& client) {
  bool ok = true;
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    client.out().integer(0);
    return;
  }

  int64_t removed = 0;
  for (size_t i = 2; i < client.argc(); ++i) {
    if (zset->remove(client.arg(i))) ++removed;
  }
  if (zset->empty()) client.db().erase(client.arg(1));
  client.out().integer(removed);
}

namespace {

void zrank_generic(Client& client, bool reverse) {
  bool with_score = false;
  if (client.argc() == 4) {
    if (!str_ieq(client.arg(3), "withscore")) {
      client.out().error(err::kSyntax);
      return;
    }
    with_score = true;
  } else if (client.argc() > 4) {
    client.out().error(err::kSyntax);
    return;
  }

  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;

  const int64_t rank = zset == nullptr ? -1 : zset->rank(client.arg(2), reverse);
  if (rank < 0) {
    // WITHSCORE replies with a null array so the shape still matches.
    if (with_score) {
      client.out().null_array();
    } else {
      client.out().null_bulk();
    }
    return;
  }
  if (!with_score) {
    client.out().integer(rank);
    return;
  }
  client.out().array(2);
  client.out().integer(rank);
  client.out().double_value(*zset->score_of(client.arg(2)));
}

}  // namespace

void cmd_zrank(Client& client) { zrank_generic(client, false); }
void cmd_zrevrank(Client& client) { zrank_generic(client, true); }

void cmd_zcount(Client& client) {
  ScoreRange range;
  if (!parse_score_range(client.arg(2), client.arg(3), &range)) {
    client.out().error(err::kMinOrMaxNotFloat);
    return;
  }

  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    client.out().integer(0);
    return;
  }

  const SkipList& list = zset->skiplist();
  const SkipList::Node* first = list.first_in_score_range(range);
  if (first == nullptr) {
    client.out().integer(0);
    return;
  }
  const SkipList::Node* last = list.last_in_score_range(range);
  // Ranks turn the count into arithmetic instead of a walk.
  const int64_t first_rank = list.rank_of(first->score, first->member);
  const int64_t last_rank = list.rank_of(last->score, last->member);
  client.out().integer(last_rank - first_rank + 1);
}

void cmd_zlexcount(Client& client) {
  LexRange range;
  if (!parse_lex_range(client.arg(2), client.arg(3), &range)) {
    client.out().error(err::kMinOrMaxNotValid);
    return;
  }

  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    client.out().integer(0);
    return;
  }

  const SkipList& list = zset->skiplist();
  const SkipList::Node* first = list.first_in_lex_range(range);
  if (first == nullptr) {
    client.out().integer(0);
    return;
  }
  const SkipList::Node* last = list.last_in_lex_range(range);
  const int64_t first_rank = list.rank_of(first->score, first->member);
  const int64_t last_rank = list.rank_of(last->score, last->member);
  client.out().integer(last_rank - first_rank + 1);
}

// --- ZRANGE and friends -------------------------------------------------------

namespace {

enum class RangeBy { Rank, Score, Lex };

// Collects the elements of a range query in reply order, applying LIMIT.
// Returns false if an error has already been sent.
bool collect_range(Client& client, const ZSet* zset, RangeBy by, std::string_view min_arg,
                   std::string_view max_arg, bool reverse, bool has_limit, int64_t offset,
                   int64_t count, std::vector<ScoredMember>* out) {
  if (by == RangeBy::Rank) {
    int64_t start = 0;
    int64_t end = 0;
    if (!get_int64_arg(client, min_arg, &start)) return false;
    if (!get_int64_arg(client, max_arg, &end)) return false;
    if (zset == nullptr) return true;

    const auto length = static_cast<int64_t>(zset->size());
    normalize_range(length, &start, &end);
    if (start > end || start >= length) return true;

    const SkipList& list = zset->skiplist();
    for (int64_t rank = start; rank <= end; ++rank) {
      // Reverse rank queries index from the far end.
      const int64_t actual = reverse ? length - 1 - rank : rank;
      const SkipList::Node* node = list.at_rank(actual);
      if (node == nullptr) break;
      out->push_back({node->member, node->score});
    }
    return true;
  }

  if (by == RangeBy::Score) {
    ScoreRange range;
    // For reverse queries the caller passes max first, so swap back.
    const std::string_view low = reverse ? max_arg : min_arg;
    const std::string_view high = reverse ? min_arg : max_arg;
    if (!parse_score_range(low, high, &range)) {
      client.out().error(err::kMinOrMaxNotFloat);
      return false;
    }
    if (zset == nullptr) return true;

    const SkipList& list = zset->skiplist();
    const SkipList::Node* node =
        reverse ? list.last_in_score_range(range) : list.first_in_score_range(range);

    while (node != nullptr && offset > 0) {
      node = reverse ? node->backward : node->levels[0].forward;
      if (node != nullptr && !list.score_in_range(node->score, range)) node = nullptr;
      --offset;
    }
    while (node != nullptr) {
      if (has_limit && count >= 0 && static_cast<int64_t>(out->size()) >= count) break;
      out->push_back({node->member, node->score});
      node = reverse ? node->backward : node->levels[0].forward;
      if (node != nullptr && !list.score_in_range(node->score, range)) break;
    }
    return true;
  }

  LexRange range;
  const std::string_view low = reverse ? max_arg : min_arg;
  const std::string_view high = reverse ? min_arg : max_arg;
  if (!parse_lex_range(low, high, &range)) {
    client.out().error(err::kMinOrMaxNotValid);
    return false;
  }
  if (zset == nullptr) return true;

  const SkipList& list = zset->skiplist();
  const SkipList::Node* node =
      reverse ? list.last_in_lex_range(range) : list.first_in_lex_range(range);

  while (node != nullptr && offset > 0) {
    node = reverse ? node->backward : node->levels[0].forward;
    if (node != nullptr && !list.member_in_lex_range(node->member, range)) node = nullptr;
    --offset;
  }
  while (node != nullptr) {
    if (has_limit && count >= 0 && static_cast<int64_t>(out->size()) >= count) break;
    out->push_back({node->member, node->score});
    node = reverse ? node->backward : node->levels[0].forward;
    if (node != nullptr && !list.member_in_lex_range(node->member, range)) break;
  }
  return true;
}

// Shared driver for ZRANGE/ZREVRANGE/ZRANGEBYSCORE/ZRANGEBYLEX/ZRANGESTORE.
void range_command(Client& client, RangeBy by, bool reverse, bool allow_options, bool store) {
  const size_t key_index = store ? 2 : 1;
  const size_t min_index = key_index + 1;
  const size_t max_index = key_index + 2;

  bool with_scores = false;
  bool has_limit = false;
  int64_t offset = 0;
  int64_t count = -1;

  for (size_t i = max_index + 1; i < client.argc(); ++i) {
    const std::string& option = client.arg(i);
    if (str_ieq(option, "withscores") && by != RangeBy::Lex && !store) {
      with_scores = true;
    } else if (allow_options && str_ieq(option, "byscore")) {
      by = RangeBy::Score;
    } else if (allow_options && str_ieq(option, "bylex")) {
      by = RangeBy::Lex;
    } else if (allow_options && str_ieq(option, "rev")) {
      reverse = true;
    } else if (str_ieq(option, "limit") && i + 2 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), &offset)) return;
      if (!get_int64_arg(client, client.arg(i + 2), &count)) return;
      has_limit = true;
      i += 2;
    } else {
      client.out().error(err::kSyntax);
      return;
    }
  }

  if (has_limit && by == RangeBy::Rank) {
    client.out().error(
        "ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
    return;
  }
  if (offset < 0) {
    // A negative offset yields nothing; Redis treats it as an empty result.
    if (store) {
      client.db().erase(client.arg(1));
      client.out().integer(0);
    } else {
      client.out().array(0);
    }
    return;
  }

  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(key_index), &ok);
  if (!ok) return;

  std::vector<ScoredMember> items;
  if (!collect_range(client, zset, by, client.arg(min_index), client.arg(max_index), reverse,
                     has_limit, offset, count, &items)) {
    return;
  }

  if (!store) {
    reply_scored(client, items, with_scores);
    return;
  }

  const std::string& destination = client.arg(1);
  if (items.empty()) {
    client.db().erase(destination);
    client.out().integer(0);
    return;
  }
  Object stored = Object::make_zset();
  for (const ScoredMember& item : items) stored.as_zset().add(item.member, item.score);
  client.db().set(destination, std::move(stored));
  client.out().integer(static_cast<int64_t>(items.size()));
}

}  // namespace

void cmd_zrange(Client& client) {
  range_command(client, RangeBy::Rank, false, /*allow_options=*/true, /*store=*/false);
}
void cmd_zrevrange(Client& client) {
  range_command(client, RangeBy::Rank, true, /*allow_options=*/false, /*store=*/false);
}
void cmd_zrangebyscore(Client& client) {
  range_command(client, RangeBy::Score, false, /*allow_options=*/false, /*store=*/false);
}
void cmd_zrevrangebyscore(Client& client) {
  range_command(client, RangeBy::Score, true, /*allow_options=*/false, /*store=*/false);
}
void cmd_zrangebylex(Client& client) {
  range_command(client, RangeBy::Lex, false, /*allow_options=*/false, /*store=*/false);
}
void cmd_zrevrangebylex(Client& client) {
  range_command(client, RangeBy::Lex, true, /*allow_options=*/false, /*store=*/false);
}
void cmd_zrangestore(Client& client) {
  range_command(client, RangeBy::Rank, false, /*allow_options=*/true, /*store=*/true);
}

// --- ZREMRANGEBY* -------------------------------------------------------------

namespace {

void zremrange_generic(Client& client, RangeBy by) {
  bool ok = true;
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;

  std::vector<ScoredMember> doomed;
  if (!collect_range(client, zset, by, client.arg(2), client.arg(3), /*reverse=*/false,
                     /*has_limit=*/false, 0, -1, &doomed)) {
    return;
  }
  if (zset == nullptr) {
    client.out().integer(0);
    return;
  }

  // Collected first, then removed: mutating while walking the skiplist would
  // invalidate the node being followed.
  int64_t removed = 0;
  for (const ScoredMember& item : doomed) {
    if (zset->remove(item.member)) ++removed;
  }
  if (zset->empty()) client.db().erase(client.arg(1));
  client.out().integer(removed);
}

}  // namespace

void cmd_zremrangebyrank(Client& client) { zremrange_generic(client, RangeBy::Rank); }
void cmd_zremrangebyscore(Client& client) { zremrange_generic(client, RangeBy::Score); }
void cmd_zremrangebylex(Client& client) { zremrange_generic(client, RangeBy::Lex); }

// --- ZPOPMIN / ZPOPMAX / ZMPOP / ZRANDMEMBER ----------------------------------

namespace {

void zpop_generic(Client& client, bool from_min) {
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
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    client.out().array(0);
    return;
  }

  const auto wanted = std::min<size_t>(static_cast<size_t>(count), zset->size());
  std::vector<ScoredMember> popped;
  popped.reserve(wanted);
  for (size_t i = 0; i < wanted; ++i) {
    const SkipList::Node* node = from_min ? zset->skiplist().first() : zset->skiplist().last();
    if (node == nullptr) break;
    popped.push_back({node->member, node->score});
    zset->remove(popped.back().member);
  }
  if (zset->empty()) client.db().erase(client.arg(1));

  // Without a count the reply is a flat member/score pair, not an array of pairs.
  if (!has_count) {
    if (popped.empty()) {
      client.out().array(0);
      return;
    }
    client.out().array(2);
    client.out().bulk(popped[0].member);
    client.out().double_value(popped[0].score);
    return;
  }
  reply_scored(client, popped, /*with_scores=*/true);
}

}  // namespace

void cmd_zpopmin(Client& client) { zpop_generic(client, true); }
void cmd_zpopmax(Client& client) { zpop_generic(client, false); }

void cmd_zmpop(Client& client) {
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
  bool from_min = false;
  if (str_ieq(client.arg(where_index), "min")) from_min = true;
  else if (!str_ieq(client.arg(where_index), "max")) {
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

  for (int64_t i = 0; i < numkeys; ++i) {
    const std::string& key = client.arg(static_cast<size_t>(i) + 2);
    bool ok = true;
    ZSet* zset = lookup_zset(client, key, &ok);
    if (!ok) return;
    if (zset == nullptr || zset->empty()) continue;

    const auto wanted = std::min<size_t>(static_cast<size_t>(count), zset->size());
    std::vector<ScoredMember> popped;
    for (size_t n = 0; n < wanted; ++n) {
      const SkipList::Node* node = from_min ? zset->skiplist().first() : zset->skiplist().last();
      if (node == nullptr) break;
      popped.push_back({node->member, node->score});
      zset->remove(popped.back().member);
    }
    if (zset->empty()) client.db().erase(key);

    client.out().array(2);
    client.out().bulk(key);
    client.out().array(static_cast<int64_t>(popped.size()));
    for (const ScoredMember& item : popped) {
      client.out().array(2);
      client.out().bulk(item.member);
      client.out().double_value(item.score);
    }
    return;
  }
  client.out().null_array();
}

void cmd_zrandmember(Client& client) {
  const bool has_count = client.argc() >= 3;
  int64_t count = 1;
  bool with_scores = false;
  if (has_count) {
    if (!get_int64_arg(client, client.arg(2), &count)) return;
    if (client.argc() == 4) {
      if (!str_ieq(client.arg(3), "withscores")) {
        client.out().error(err::kSyntax);
        return;
      }
      with_scores = true;
    } else if (client.argc() > 4) {
      client.out().error(err::kSyntax);
      return;
    }
  }

  bool ok = true;
  ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr || zset->empty()) {
    if (has_count) {
      client.out().array(0);
    } else {
      client.out().null_bulk();
    }
    return;
  }

  if (!has_count) {
    client.out().bulk(zset->dict().random_entry()->key);
    return;
  }

  std::vector<ScoredMember> items;
  if (count < 0) {
    // Negative count allows repeats and returns exactly |count| elements.
    const int64_t wanted = -count;
    for (int64_t i = 0; i < wanted; ++i) {
      auto* entry = zset->dict().random_entry();
      items.push_back({entry->key, entry->value});
    }
  } else {
    std::vector<ScoredMember> all;
    all.reserve(zset->size());
    zset->dict().for_each(
        [&](const std::string& member, double score) { all.push_back({member, score}); });
    const auto wanted = std::min<size_t>(static_cast<size_t>(count), all.size());
    items.assign(all.begin(), all.begin() + static_cast<long>(wanted));
  }
  reply_scored(client, items, with_scores);
}

// --- ZUNION / ZINTER / ZDIFF --------------------------------------------------

namespace {

enum class ZSetOp { Union, Intersect, Difference };

// Accumulates weighted scores across the source sets. Returns false if an error
// has already been sent.
bool compute_zset_op(Client& client, ZSetOp op, size_t numkeys_index, bool store,
                     std::vector<ScoredMember>* result, bool* want_scores, int64_t* limit) {
  int64_t numkeys = 0;
  if (!get_int64_arg(client, client.arg(numkeys_index), &numkeys)) return false;
  if (numkeys <= 0) {
    client.out().error("ERR at least 1 input key is needed for the given command");
    return false;
  }
  const size_t first_key = numkeys_index + 1;
  if (first_key + static_cast<size_t>(numkeys) > client.argc()) {
    client.out().error(err::kSyntax);
    return false;
  }

  std::vector<double> weights(static_cast<size_t>(numkeys), 1.0);
  enum class Aggregate { Sum, Min, Max } aggregate = Aggregate::Sum;

  for (size_t i = first_key + static_cast<size_t>(numkeys); i < client.argc(); ++i) {
    if (str_ieq(client.arg(i), "weights")) {
      if (i + static_cast<size_t>(numkeys) >= client.argc()) {
        client.out().error(err::kSyntax);
        return false;
      }
      for (int64_t w = 0; w < numkeys; ++w) {
        if (!get_double_arg(client, client.arg(i + 1 + static_cast<size_t>(w)),
                            &weights[static_cast<size_t>(w)],
                            "ERR weight value is not a float")) {
          return false;
        }
      }
      i += static_cast<size_t>(numkeys);
    } else if (str_ieq(client.arg(i), "aggregate") && i + 1 < client.argc()) {
      if (str_ieq(client.arg(i + 1), "sum")) aggregate = Aggregate::Sum;
      else if (str_ieq(client.arg(i + 1), "min")) aggregate = Aggregate::Min;
      else if (str_ieq(client.arg(i + 1), "max")) aggregate = Aggregate::Max;
      else {
        client.out().error(err::kSyntax);
        return false;
      }
      ++i;
    } else if (!store && str_ieq(client.arg(i), "withscores")) {
      if (want_scores == nullptr) {
        client.out().error(err::kSyntax);
        return false;
      }
      *want_scores = true;
    } else if (limit != nullptr && str_ieq(client.arg(i), "limit") && i + 1 < client.argc()) {
      if (!get_int64_arg(client, client.arg(i + 1), limit)) return false;
      if (*limit < 0) {
        client.out().error("ERR LIMIT can't be negative");
        return false;
      }
      ++i;
    } else {
      client.out().error(err::kSyntax);
      return false;
    }
  }

  // Snapshot each source, accepting sets as sorted sets with score 1 the way
  // Redis does.
  std::vector<std::vector<ScoredMember>> sources;
  sources.reserve(static_cast<size_t>(numkeys));
  for (int64_t k = 0; k < numkeys; ++k) {
    Object* object = client.db().lookup_read(client.arg(first_key + static_cast<size_t>(k)));
    std::vector<ScoredMember> members;
    if (object != nullptr) {
      if (object->is(ObjectType::ZSet)) {
        object->as_zset().dict().for_each(
            [&](const std::string& member, double score) { members.push_back({member, score}); });
      } else if (object->is(ObjectType::Set)) {
        object->as_set().for_each(
            [&](const std::string& member, const Empty&) { members.push_back({member, 1.0}); });
      } else {
        reply_wrong_type(client);
        return false;
      }
    }
    sources.push_back(std::move(members));
  }

  auto combine = [aggregate](double current, double incoming) {
    switch (aggregate) {
      case Aggregate::Sum: {
        const double sum = current + incoming;
        // Redis defines inf + -inf as 0 here rather than propagating NaN.
        return std::isnan(sum) ? 0.0 : sum;
      }
      case Aggregate::Min: return std::min(current, incoming);
      case Aggregate::Max: return std::max(current, incoming);
    }
    return current;
  };

  if (op == ZSetOp::Difference) {
    for (const ScoredMember& item : sources[0]) {
      bool excluded = false;
      for (size_t k = 1; k < sources.size() && !excluded; ++k) {
        for (const ScoredMember& other : sources[k]) {
          if (other.member == item.member) {
            excluded = true;
            break;
          }
        }
      }
      if (!excluded) result->push_back(item);
    }
    return true;
  }

  // Accumulate into a dict keyed by member, tracking how many sources each
  // member appeared in so intersection can filter at the end.
  Dict<std::pair<double, int>> accumulator;
  for (size_t k = 0; k < sources.size(); ++k) {
    const double weight = weights[k];
    for (const ScoredMember& item : sources[k]) {
      double weighted = item.score * weight;
      if (std::isnan(weighted)) weighted = 0.0;
      auto* slot = accumulator.find(item.member);
      if (slot == nullptr) {
        accumulator.insert(item.member, std::make_pair(weighted, 1));
      } else {
        slot->first = combine(slot->first, weighted);
        ++slot->second;
      }
    }
  }

  const int required = op == ZSetOp::Intersect ? static_cast<int>(sources.size()) : 1;
  accumulator.for_each([&](const std::string& member, const std::pair<double, int>& value) {
    if (value.second >= required) result->push_back({member, value.first});
  });

  // Results are ordered by score, then member, like a real sorted set.
  std::sort(result->begin(), result->end(), [](const ScoredMember& a, const ScoredMember& b) {
    if (a.score != b.score) return a.score < b.score;
    return a.member < b.member;
  });
  return true;
}

void zset_op_command(Client& client, ZSetOp op, bool store) {
  std::vector<ScoredMember> result;
  bool with_scores = false;
  if (!compute_zset_op(client, op, store ? 2 : 1, store, &result, &with_scores, nullptr)) return;

  if (!store) {
    reply_scored(client, result, with_scores);
    return;
  }

  const std::string& destination = client.arg(1);
  if (result.empty()) {
    client.db().erase(destination);
    client.out().integer(0);
    return;
  }
  Object stored = Object::make_zset();
  for (const ScoredMember& item : result) stored.as_zset().add(item.member, item.score);
  client.db().set(destination, std::move(stored));
  client.out().integer(static_cast<int64_t>(result.size()));
}

}  // namespace

void cmd_zunion(Client& client) { zset_op_command(client, ZSetOp::Union, false); }
void cmd_zinter(Client& client) { zset_op_command(client, ZSetOp::Intersect, false); }
void cmd_zdiff(Client& client) { zset_op_command(client, ZSetOp::Difference, false); }
void cmd_zunionstore(Client& client) { zset_op_command(client, ZSetOp::Union, true); }
void cmd_zinterstore(Client& client) { zset_op_command(client, ZSetOp::Intersect, true); }
void cmd_zdiffstore(Client& client) { zset_op_command(client, ZSetOp::Difference, true); }

void cmd_zintercard(Client& client) {
  std::vector<ScoredMember> result;
  int64_t limit = 0;
  if (!compute_zset_op(client, ZSetOp::Intersect, 1, false, &result, nullptr, &limit)) return;
  const auto size = static_cast<int64_t>(result.size());
  client.out().integer(limit > 0 ? std::min(limit, size) : size);
}

void cmd_zscan(Client& client) {
  ScanOptions options;
  if (!parse_scan_options(client, 3, /*allow_novalues=*/false, &options)) return;

  bool ok = true;
  const ZSet* zset = lookup_zset(client, client.arg(1), &ok);
  if (!ok) return;
  if (zset == nullptr) {
    client.out().array(2);
    client.out().bulk("0");
    client.out().array(0);
    return;
  }

  std::vector<ScoredMember> collected;
  uint64_t cursor = options.cursor;
  int64_t max_iterations = options.count * 10;
  do {
    cursor = zset->dict().scan(cursor, [&](const std::string& member, double score) {
      collected.push_back({member, score});
    });
  } while (cursor != 0 && --max_iterations > 0 &&
           static_cast<int64_t>(collected.size()) < options.count);

  std::vector<ScoredMember> matched;
  for (ScoredMember& item : collected) {
    if (scan_matches(options, item.member)) matched.push_back(std::move(item));
  }

  client.out().array(2);
  client.out().bulk(ll2string(static_cast<int64_t>(cursor)));
  // ZSCAN always interleaves member and score, in both protocols.
  client.out().array(static_cast<int64_t>(matched.size()) * 2);
  for (const ScoredMember& item : matched) {
    client.out().bulk(item.member);
    client.out().bulk(d2string(item.score));
  }
}

}  // namespace credis
