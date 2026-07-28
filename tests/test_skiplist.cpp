#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "harness.h"
#include "store/skiplist.h"
#include "util/strings.h"

using namespace credis;

namespace {

// Reads the list front to back.
std::vector<std::string> members_in_order(const SkipList& list) {
  std::vector<std::string> out;
  for (const auto* node = list.first(); node != nullptr; node = node->levels[0].forward) {
    out.push_back(node->member);
  }
  return out;
}

// Reads the list back to front, which must be the exact reverse.
std::vector<std::string> members_reversed(const SkipList& list) {
  std::vector<std::string> out;
  for (const auto* node = list.last(); node != nullptr; node = node->backward) {
    out.push_back(node->member);
  }
  return out;
}

}  // namespace

TEST(SkipList, InsertKeepsScoreOrder) {
  SkipList list;
  list.insert(3, "c");
  list.insert(1, "a");
  list.insert(2, "b");

  CHECK_EQ(list.size(), 3u);
  CHECK_EQ(members_in_order(list), (std::vector<std::string>{"a", "b", "c"}));

  auto backward = members_reversed(list);
  std::reverse(backward.begin(), backward.end());
  CHECK_EQ(backward, members_in_order(list));
}

TEST(SkipList, TiesBreakOnMemberBytes) {
  SkipList list;
  list.insert(1, "b");
  list.insert(1, "a");
  list.insert(1, "c");
  CHECK_EQ(members_in_order(list), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(SkipList, RemoveOnlyMatchesExactPair) {
  SkipList list;
  list.insert(1, "a");
  list.insert(2, "b");

  CHECK_FALSE(list.remove(99, "a"));  // wrong score
  CHECK_FALSE(list.remove(1, "zz"));  // wrong member
  CHECK(list.remove(1, "a"));
  CHECK_EQ(list.size(), 1u);
  CHECK_EQ(members_in_order(list), (std::vector<std::string>{"b"}));
}

TEST(SkipList, RankMatchesPosition) {
  SkipList list;
  for (int i = 0; i < 200; ++i) list.insert(i, "m" + ll2string(i));

  for (int i = 0; i < 200; ++i) {
    CHECK_EQ(list.rank_of(i, "m" + ll2string(i)), i);
    const auto* node = list.at_rank(i);
    CHECK(node != nullptr);
    CHECK_BYTES(node->member, "m" + ll2string(i));
  }
  CHECK_EQ(list.rank_of(1, "missing"), -1);
  CHECK(list.at_rank(200) == nullptr);
  CHECK(list.at_rank(-1) == nullptr);
}

TEST(SkipList, UpdateScoreRepositions) {
  SkipList list;
  list.insert(1, "a");
  list.insert(2, "b");
  list.insert(3, "c");

  list.update_score(1, "a", 10);
  CHECK_EQ(members_in_order(list), (std::vector<std::string>{"b", "c", "a"}));
  CHECK_EQ(list.rank_of(10, "a"), 2);
  CHECK_EQ(list.size(), 3u);
}

TEST(SkipList, ScoreRangeBoundaries) {
  SkipList list;
  for (int i = 1; i <= 5; ++i) list.insert(i, "m" + ll2string(i));

  ScoreRange range;
  CHECK(parse_score_range("2", "4", &range));
  CHECK_BYTES(list.first_in_score_range(range)->member, "m2");
  CHECK_BYTES(list.last_in_score_range(range)->member, "m4");

  // Exclusive bounds skip the endpoints.
  CHECK(parse_score_range("(2", "(4", &range));
  CHECK_BYTES(list.first_in_score_range(range)->member, "m3");
  CHECK_BYTES(list.last_in_score_range(range)->member, "m3");

  CHECK(parse_score_range("-inf", "+inf", &range));
  CHECK_BYTES(list.first_in_score_range(range)->member, "m1");
  CHECK_BYTES(list.last_in_score_range(range)->member, "m5");

  // Ranges entirely outside the data must report nothing.
  CHECK(parse_score_range("10", "20", &range));
  CHECK(list.first_in_score_range(range) == nullptr);
  CHECK(list.last_in_score_range(range) == nullptr);
  CHECK(parse_score_range("-20", "-10", &range));
  CHECK(list.first_in_score_range(range) == nullptr);
  CHECK(list.last_in_score_range(range) == nullptr);

  // An inverted range is empty by definition.
  CHECK(parse_score_range("4", "2", &range));
  CHECK(list.first_in_score_range(range) == nullptr);
}

TEST(SkipList, LexRangeBoundaries) {
  SkipList list;
  for (const char* m : {"a", "b", "c", "d", "e"}) list.insert(0, m);

  LexRange range;
  CHECK(parse_lex_range("-", "+", &range));
  CHECK_BYTES(list.first_in_lex_range(range)->member, "a");
  CHECK_BYTES(list.last_in_lex_range(range)->member, "e");

  CHECK(parse_lex_range("[b", "[d", &range));
  CHECK_BYTES(list.first_in_lex_range(range)->member, "b");
  CHECK_BYTES(list.last_in_lex_range(range)->member, "d");

  CHECK(parse_lex_range("(b", "(d", &range));
  CHECK_BYTES(list.first_in_lex_range(range)->member, "c");
  CHECK_BYTES(list.last_in_lex_range(range)->member, "c");

  CHECK(parse_lex_range("[x", "[z", &range));
  CHECK(list.first_in_lex_range(range) == nullptr);

  // Bounds must carry an explicit '[', '(', '-' or '+' marker.
  CHECK_FALSE(parse_lex_range("b", "d", &range));
  CHECK_FALSE(parse_lex_range("+", "[d", &range));
  CHECK_FALSE(parse_lex_range("[b", "-", &range));
}

TEST(SkipList, ParseScoreRangeRejectsGarbage) {
  ScoreRange range;
  CHECK_FALSE(parse_score_range("abc", "5", &range));
  CHECK_FALSE(parse_score_range("5", "xyz", &range));
  CHECK_FALSE(parse_score_range("(", "5", &range));
  CHECK(parse_score_range("(1.5", "2.5", &range));
  CHECK(range.min_exclusive);
  CHECK_FALSE(range.max_exclusive);
}

// Cross-checks the skiplist against a sorted vector across thousands of
// randomized inserts, deletes and score updates.
TEST(SkipList, MatchesSortedVectorOracle) {
  SkipList list;
  std::vector<std::pair<double, std::string>> oracle;
  std::mt19937 rng(12345);

  auto oracle_insert = [&](double score, const std::string& member) {
    oracle.emplace_back(score, member);
    std::sort(oracle.begin(), oracle.end());
  };
  auto oracle_erase = [&](double score, const std::string& member) {
    auto it = std::find(oracle.begin(), oracle.end(), std::make_pair(score, member));
    if (it != oracle.end()) oracle.erase(it);
  };

  for (int step = 0; step < 4000; ++step) {
    const int action = static_cast<int>(rng() % 10);
    const auto score = static_cast<double>(rng() % 50);
    const std::string member = "m" + ll2string(static_cast<int64_t>(rng() % 300));

    if (action < 6) {
      // Insert only if this exact member is absent, mirroring a sorted set.
      const bool present = std::any_of(oracle.begin(), oracle.end(),
                                       [&](const auto& e) { return e.second == member; });
      if (!present) {
        list.insert(score, member);
        oracle_insert(score, member);
      }
    } else if (action < 9) {
      auto it = std::find_if(oracle.begin(), oracle.end(),
                             [&](const auto& e) { return e.second == member; });
      if (it != oracle.end()) {
        list.remove(it->first, member);
        oracle_erase(it->first, member);
      }
    } else {
      auto it = std::find_if(oracle.begin(), oracle.end(),
                             [&](const auto& e) { return e.second == member; });
      if (it != oracle.end()) {
        const double old_score = it->first;
        list.update_score(old_score, member, score);
        oracle_erase(old_score, member);
        oracle_insert(score, member);
      }
    }

    CHECK_EQ(list.size(), oracle.size());
  }

  // Final structure must agree with the oracle element for element.
  std::vector<std::string> expected;
  for (const auto& [score, member] : oracle) expected.push_back(member);
  CHECK_EQ(members_in_order(list), expected);

  auto backward = members_reversed(list);
  std::reverse(backward.begin(), backward.end());
  CHECK_EQ(backward, expected);

  // Every span-derived rank must match the oracle's index.
  for (size_t i = 0; i < oracle.size(); ++i) {
    CHECK_EQ(list.rank_of(oracle[i].first, oracle[i].second), static_cast<int64_t>(i));
    const auto* node = list.at_rank(static_cast<int64_t>(i));
    CHECK(node != nullptr);
    CHECK_BYTES(node->member, oracle[i].second);
  }
}

TEST(ZSet, AddUpdatesAndRanks) {
  ZSet zset;
  CHECK(zset.add("a", 1));
  CHECK(zset.add("b", 2));
  CHECK_FALSE(zset.add("a", 5));  // already present: an update, not an add
  CHECK_EQ(zset.size(), 2u);
  CHECK_EQ(*zset.score_of("a"), 5.0);

  // 'a' moved above 'b'.
  CHECK_EQ(zset.rank("b", false), 0);
  CHECK_EQ(zset.rank("a", false), 1);
  CHECK_EQ(zset.rank("a", true), 0);
  CHECK_EQ(zset.rank("missing", false), -1);

  CHECK(zset.remove("a"));
  CHECK_FALSE(zset.remove("a"));
  CHECK_EQ(zset.size(), 1u);
  CHECK_EQ(zset.skiplist().size(), 1u);
}

TEST(ZSet, DictAndSkipListStayInSync) {
  ZSet zset;
  std::mt19937 rng(999);
  for (int i = 0; i < 2000; ++i) {
    zset.add("m" + ll2string(static_cast<int64_t>(rng() % 500)),
             static_cast<double>(rng() % 100));
  }
  CHECK_EQ(zset.size(), zset.skiplist().size());

  // Every skiplist node's score must match the dict's copy.
  for (const auto* node = zset.skiplist().first(); node != nullptr;
       node = node->levels[0].forward) {
    const double* score = zset.score_of(node->member);
    CHECK(score != nullptr);
    CHECK_EQ(*score, node->score);
  }

  for (int i = 0; i < 500; ++i) zset.remove("m" + ll2string(i));
  CHECK_EQ(zset.size(), 0u);
  CHECK_EQ(zset.skiplist().size(), 0u);
}
