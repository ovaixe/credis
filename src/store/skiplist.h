#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "store/dict.h"

namespace credis {

// A score range, as parsed from ZRANGEBYSCORE's "min"/"max" arguments.
// "(5" is an exclusive bound, "5" inclusive, "-inf"/"+inf" the open ends.
struct ScoreRange {
  double min = 0;
  double max = 0;
  bool min_exclusive = false;
  bool max_exclusive = false;

  bool empty() const {
    return min > max || (min == max && (min_exclusive || max_exclusive));
  }
};

// A lexicographic range for ZRANGEBYLEX. Only meaningful when every element
// shares the same score.
//   "-"     lowest possible string        "+"     highest possible string
//   "[abc"  inclusive                     "(abc"  exclusive
struct LexRange {
  std::string min;
  std::string max;
  bool min_exclusive = false;
  bool max_exclusive = false;
  bool min_infinite = false;  // "-"
  bool max_infinite = false;  // "+"
};

// Parsers returning false on malformed input.
bool parse_score_range(std::string_view min, std::string_view max, ScoreRange* out);
bool parse_lex_range(std::string_view min, std::string_view max, LexRange* out);

// The ordered half of a sorted set.
//
// A skiplist rather than a balanced tree because it supports the operations
// Redis needs with far less code: ordered traversal in both directions, and
// O(log n) rank queries thanks to a "span" count on every forward pointer that
// records how many nodes that pointer jumps over.
//
// Elements are ordered by (score, member), with members compared bytewise.
class SkipList {
 public:
  static constexpr int kMaxLevel = 32;

  struct Node {
    struct Level {
      Node* forward = nullptr;
      // Number of nodes this pointer skips. Summing spans along a search path
      // yields a rank, which is what makes ZRANK O(log n) instead of O(n).
      uint64_t span = 0;
    };

    std::string member;
    double score = 0;
    Node* backward = nullptr;
    std::vector<Level> levels;

    Node(int level, double node_score, std::string node_member)
        : member(std::move(node_member)), score(node_score), levels(static_cast<size_t>(level)) {}
  };

  SkipList();
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;
  SkipList(SkipList&& other) noexcept;
  SkipList& operator=(SkipList&& other) noexcept;

  size_t size() const { return length_; }
  bool empty() const { return length_ == 0; }

  // First and last elements in order, or nullptr when empty.
  Node* first() const { return header_->levels[0].forward; }
  Node* last() const { return tail_; }

  Node* insert(double score, std::string member);
  // Removes (score, member). Returns false if that exact pair is absent.
  bool remove(double score, std::string_view member);
  // Re-positions an existing member under a new score.
  Node* update_score(double old_score, std::string_view member, double new_score);

  // 0-based rank of (score, member), or -1 when absent.
  int64_t rank_of(double score, std::string_view member) const;
  // Element at a 0-based rank, or nullptr when out of range.
  Node* at_rank(int64_t rank) const;

  // Range boundaries. Return nullptr when no element falls inside the range.
  Node* first_in_score_range(const ScoreRange& range) const;
  Node* last_in_score_range(const ScoreRange& range) const;
  Node* first_in_lex_range(const LexRange& range) const;
  Node* last_in_lex_range(const LexRange& range) const;

  bool score_in_range(double score, const ScoreRange& range) const;
  bool member_in_lex_range(std::string_view member, const LexRange& range) const;
  // True when at least one element could fall inside `range`; lets the range
  // walks bail out before descending the levels.
  bool overlaps_score_range(const ScoreRange& range) const;

 private:
  // Compares (score, member) against a node; true when the node sorts first.
  static bool node_less(const Node* node, double score, std::string_view member);
  static int random_level();

  void init();
  void destroy();

  Node* header_ = nullptr;  // sentinel; holds no element
  Node* tail_ = nullptr;
  size_t length_ = 0;
  int level_ = 1;  // highest level currently in use
};

// A sorted set: a skiplist for ordering plus a dict for O(1) score lookup.
// Both structures always hold exactly the same members.
class ZSet {
 public:
  ZSet() = default;
  ZSet(ZSet&&) = default;
  ZSet& operator=(ZSet&&) = default;
  ZSet(const ZSet&) = delete;
  ZSet& operator=(const ZSet&) = delete;

  size_t size() const { return dict_.size(); }
  bool empty() const { return dict_.empty(); }

  const double* score_of(std::string_view member) const { return dict_.find(member); }
  bool contains(std::string_view member) const { return dict_.contains(member); }

  // Adds or repositions `member`. Returns true when it was newly added.
  bool add(const std::string& member, double score);
  bool remove(std::string_view member);

  // 0-based rank, ascending or descending; -1 when the member is absent.
  int64_t rank(std::string_view member, bool reverse) const;

  const SkipList& skiplist() const { return skiplist_; }
  SkipList& skiplist() { return skiplist_; }
  const Dict<double>& dict() const { return dict_; }
  Dict<double>& dict() { return dict_; }

 private:
  Dict<double> dict_;
  SkipList skiplist_;
};

}  // namespace credis
