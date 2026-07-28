#include "store/skiplist.h"

#include <cmath>
#include <limits>
#include <random>

#include "util/strings.h"

namespace credis {
namespace {

// Redis uses p = 0.25, which gives an average of 1/(1-p) = 1.33 pointers per
// node while keeping search O(log n).
constexpr uint32_t kLevelThreshold = static_cast<uint32_t>(0.25 * 0xFFFF);

uint32_t next_random16() {
  static thread_local std::mt19937 engine(0x5eed1234u);
  return engine() & 0xFFFF;
}

}  // namespace

bool parse_score_range(std::string_view min, std::string_view max, ScoreRange* out) {
  auto parse_one = [](std::string_view text, double* value, bool* exclusive) {
    *exclusive = false;
    if (!text.empty() && text.front() == '(') {
      *exclusive = true;
      text.remove_prefix(1);
    }
    if (text == "inf" || text == "+inf" || text == "infinity" || text == "+infinity") {
      *value = std::numeric_limits<double>::infinity();
      return true;
    }
    if (text == "-inf" || text == "-infinity") {
      *value = -std::numeric_limits<double>::infinity();
      return true;
    }
    return string2d(text, value);
  };

  return parse_one(min, &out->min, &out->min_exclusive) &&
         parse_one(max, &out->max, &out->max_exclusive);
}

bool parse_lex_range(std::string_view min, std::string_view max, LexRange* out) {
  auto parse_one = [](std::string_view text, std::string* value, bool* exclusive,
                      bool* infinite, bool is_min) {
    *exclusive = false;
    *infinite = false;
    if (text.size() == 1 && text[0] == '-') {
      // "-" is the lowest possible element for a min bound only.
      *infinite = is_min;
      return is_min;
    }
    if (text.size() == 1 && text[0] == '+') {
      *infinite = !is_min;
      return !is_min;
    }
    if (text.empty()) return false;
    if (text[0] == '[') {
      value->assign(text.substr(1));
      return true;
    }
    if (text[0] == '(') {
      *exclusive = true;
      value->assign(text.substr(1));
      return true;
    }
    // Anything else is malformed: lex bounds must carry an explicit marker.
    return false;
  };

  return parse_one(min, &out->min, &out->min_exclusive, &out->min_infinite, true) &&
         parse_one(max, &out->max, &out->max_exclusive, &out->max_infinite, false);
}

// --- SkipList -----------------------------------------------------------------

SkipList::SkipList() { init(); }

SkipList::~SkipList() { destroy(); }

SkipList::SkipList(SkipList&& other) noexcept
    : header_(other.header_), tail_(other.tail_), length_(other.length_), level_(other.level_) {
  other.header_ = nullptr;
  other.tail_ = nullptr;
  other.length_ = 0;
  other.level_ = 1;
  other.init();
}

SkipList& SkipList::operator=(SkipList&& other) noexcept {
  if (this != &other) {
    destroy();
    header_ = other.header_;
    tail_ = other.tail_;
    length_ = other.length_;
    level_ = other.level_;
    other.header_ = nullptr;
    other.tail_ = nullptr;
    other.length_ = 0;
    other.level_ = 1;
    other.init();
  }
  return *this;
}

void SkipList::init() {
  header_ = new Node(kMaxLevel, 0, std::string());
  tail_ = nullptr;
  length_ = 0;
  level_ = 1;
}

void SkipList::destroy() {
  if (header_ == nullptr) return;
  Node* node = header_->levels[0].forward;
  while (node != nullptr) {
    Node* next = node->levels[0].forward;
    delete node;
    node = next;
  }
  delete header_;
  header_ = nullptr;
  tail_ = nullptr;
  length_ = 0;
  level_ = 1;
}

int SkipList::random_level() {
  int level = 1;
  while (next_random16() < kLevelThreshold) ++level;
  return level < kMaxLevel ? level : kMaxLevel;
}

bool SkipList::node_less(const Node* node, double score, std::string_view member) {
  if (node->score < score) return true;
  if (node->score > score) return false;
  return node->member < member;
}

SkipList::Node* SkipList::insert(double score, std::string member) {
  Node* update[kMaxLevel];
  uint64_t rank[kMaxLevel];
  Node* node = header_;

  // Walk down from the top level, recording where we dropped a level and how
  // many nodes were skipped getting there.
  for (int i = level_ - 1; i >= 0; --i) {
    rank[i] = (i == level_ - 1) ? 0 : rank[i + 1];
    while (node->levels[static_cast<size_t>(i)].forward != nullptr &&
           node_less(node->levels[static_cast<size_t>(i)].forward, score, member)) {
      rank[i] += node->levels[static_cast<size_t>(i)].span;
      node = node->levels[static_cast<size_t>(i)].forward;
    }
    update[i] = node;
  }

  const int level = random_level();
  if (level > level_) {
    // New levels start at the header and span the whole list.
    for (int i = level_; i < level; ++i) {
      rank[i] = 0;
      update[i] = header_;
      update[i]->levels[static_cast<size_t>(i)].span = length_;
    }
    level_ = level;
  }

  Node* inserted = new Node(level, score, std::move(member));
  for (int i = 0; i < level; ++i) {
    const auto index = static_cast<size_t>(i);
    inserted->levels[index].forward = update[i]->levels[index].forward;
    update[i]->levels[index].forward = inserted;
    // Split the span that used to cross this position.
    inserted->levels[index].span = update[i]->levels[index].span - (rank[0] - rank[i]);
    update[i]->levels[index].span = (rank[0] - rank[i]) + 1;
  }
  // Levels above the new node now cross one extra element.
  for (int i = level; i < level_; ++i) {
    ++update[i]->levels[static_cast<size_t>(i)].span;
  }

  inserted->backward = (update[0] == header_) ? nullptr : update[0];
  if (inserted->levels[0].forward != nullptr) {
    inserted->levels[0].forward->backward = inserted;
  } else {
    tail_ = inserted;
  }
  ++length_;
  return inserted;
}

bool SkipList::remove(double score, std::string_view member) {
  Node* update[kMaxLevel];
  Node* node = header_;

  for (int i = level_ - 1; i >= 0; --i) {
    while (node->levels[static_cast<size_t>(i)].forward != nullptr &&
           node_less(node->levels[static_cast<size_t>(i)].forward, score, member)) {
      node = node->levels[static_cast<size_t>(i)].forward;
    }
    update[i] = node;
  }

  node = node->levels[0].forward;
  if (node == nullptr || node->score != score || node->member != member) return false;

  for (int i = 0; i < level_; ++i) {
    const auto index = static_cast<size_t>(i);
    if (update[i]->levels[index].forward == node) {
      update[i]->levels[index].span += node->levels[index].span - 1;
      update[i]->levels[index].forward = node->levels[index].forward;
    } else {
      --update[i]->levels[index].span;
    }
  }

  if (node->levels[0].forward != nullptr) {
    node->levels[0].forward->backward = node->backward;
  } else {
    tail_ = node->backward;
  }
  // Drop levels that are now empty.
  while (level_ > 1 && header_->levels[static_cast<size_t>(level_ - 1)].forward == nullptr) {
    --level_;
  }
  --length_;
  delete node;
  return true;
}

SkipList::Node* SkipList::update_score(double old_score, std::string_view member,
                                       double new_score) {
  // Reposition by removing and reinserting; the member string is preserved.
  std::string owned(member);
  if (!remove(old_score, member)) return nullptr;
  return insert(new_score, std::move(owned));
}

int64_t SkipList::rank_of(double score, std::string_view member) const {
  const Node* node = header_;
  uint64_t rank = 0;

  for (int i = level_ - 1; i >= 0; --i) {
    while (node->levels[static_cast<size_t>(i)].forward != nullptr &&
           (node->levels[static_cast<size_t>(i)].forward->score < score ||
            (node->levels[static_cast<size_t>(i)].forward->score == score &&
             node->levels[static_cast<size_t>(i)].forward->member <= member))) {
      rank += node->levels[static_cast<size_t>(i)].span;
      node = node->levels[static_cast<size_t>(i)].forward;
    }
    // The header carries no member, so this only matches a real node.
    if (node != header_ && node->member == member) {
      return static_cast<int64_t>(rank) - 1;  // spans are 1-based; ranks are not
    }
  }
  return -1;
}

SkipList::Node* SkipList::at_rank(int64_t rank) const {
  if (rank < 0 || static_cast<uint64_t>(rank) >= length_) return nullptr;

  const uint64_t target = static_cast<uint64_t>(rank) + 1;  // spans count from 1
  Node* node = header_;
  uint64_t traversed = 0;

  for (int i = level_ - 1; i >= 0; --i) {
    while (node->levels[static_cast<size_t>(i)].forward != nullptr &&
           traversed + node->levels[static_cast<size_t>(i)].span <= target) {
      traversed += node->levels[static_cast<size_t>(i)].span;
      node = node->levels[static_cast<size_t>(i)].forward;
    }
    if (traversed == target) return node;
  }
  return nullptr;
}

bool SkipList::score_in_range(double score, const ScoreRange& range) const {
  if (range.min_exclusive ? score <= range.min : score < range.min) return false;
  if (range.max_exclusive ? score >= range.max : score > range.max) return false;
  return true;
}

bool SkipList::member_in_lex_range(std::string_view member, const LexRange& range) const {
  if (!range.min_infinite) {
    const int cmp = member.compare(range.min);
    if (range.min_exclusive ? cmp <= 0 : cmp < 0) return false;
  }
  if (!range.max_infinite) {
    const int cmp = member.compare(range.max);
    if (range.max_exclusive ? cmp >= 0 : cmp > 0) return false;
  }
  return true;
}

bool SkipList::overlaps_score_range(const ScoreRange& range) const {
  if (range.empty() || length_ == 0) return false;
  // The largest score must reach the minimum, and the smallest must not exceed
  // the maximum; otherwise the whole list sits outside the range.
  if (range.min_exclusive ? tail_->score <= range.min : tail_->score < range.min) return false;
  const Node* smallest = header_->levels[0].forward;
  if (range.max_exclusive ? smallest->score >= range.max : smallest->score > range.max) {
    return false;
  }
  return true;
}

SkipList::Node* SkipList::first_in_score_range(const ScoreRange& range) const {
  if (!overlaps_score_range(range)) return nullptr;

  Node* node = header_;
  for (int i = level_ - 1; i >= 0; --i) {
    // Advance while the next node is still below the minimum.
    while (node->levels[static_cast<size_t>(i)].forward != nullptr) {
      const double next_score = node->levels[static_cast<size_t>(i)].forward->score;
      const bool below_min = range.min_exclusive ? next_score <= range.min
                                                 : next_score < range.min;
      if (!below_min) break;
      node = node->levels[static_cast<size_t>(i)].forward;
    }
  }
  node = node->levels[0].forward;
  if (node == nullptr) return nullptr;
  if (range.max_exclusive ? node->score >= range.max : node->score > range.max) return nullptr;
  return node;
}

SkipList::Node* SkipList::last_in_score_range(const ScoreRange& range) const {
  if (!overlaps_score_range(range)) return nullptr;

  Node* node = header_;
  for (int i = level_ - 1; i >= 0; --i) {
    // Advance while the next node is still within the maximum.
    while (node->levels[static_cast<size_t>(i)].forward != nullptr) {
      const double next_score = node->levels[static_cast<size_t>(i)].forward->score;
      const bool within_max = range.max_exclusive ? next_score < range.max
                                                  : next_score <= range.max;
      if (!within_max) break;
      node = node->levels[static_cast<size_t>(i)].forward;
    }
  }
  if (node == header_) return nullptr;
  if (range.min_exclusive ? node->score <= range.min : node->score < range.min) return nullptr;
  return node;
}

SkipList::Node* SkipList::first_in_lex_range(const LexRange& range) const {
  if (length_ == 0) return nullptr;

  Node* node = header_;
  for (int i = level_ - 1; i >= 0; --i) {
    while (node->levels[static_cast<size_t>(i)].forward != nullptr) {
      Node* next = node->levels[static_cast<size_t>(i)].forward;
      bool below_min;
      if (range.min_infinite) {
        below_min = false;
      } else {
        const int cmp = next->member.compare(range.min);
        below_min = range.min_exclusive ? cmp <= 0 : cmp < 0;
      }
      if (!below_min) break;
      node = next;
    }
  }
  node = node->levels[0].forward;
  if (node == nullptr) return nullptr;
  if (!member_in_lex_range(node->member, range)) return nullptr;
  return node;
}

SkipList::Node* SkipList::last_in_lex_range(const LexRange& range) const {
  if (length_ == 0) return nullptr;

  Node* node = header_;
  for (int i = level_ - 1; i >= 0; --i) {
    while (node->levels[static_cast<size_t>(i)].forward != nullptr) {
      Node* next = node->levels[static_cast<size_t>(i)].forward;
      bool within_max;
      if (range.max_infinite) {
        within_max = true;
      } else {
        const int cmp = next->member.compare(range.max);
        within_max = range.max_exclusive ? cmp < 0 : cmp <= 0;
      }
      if (!within_max) break;
      node = next;
    }
  }
  if (node == header_) return nullptr;
  if (!member_in_lex_range(node->member, range)) return nullptr;
  return node;
}

// --- ZSet ---------------------------------------------------------------------

bool ZSet::add(const std::string& member, double score) {
  double* existing = dict_.find(member);
  if (existing != nullptr) {
    if (*existing != score) {
      skiplist_.update_score(*existing, member, score);
      *existing = score;
    }
    return false;
  }
  skiplist_.insert(score, member);
  dict_.insert(member, score);
  return true;
}

bool ZSet::remove(std::string_view member) {
  const double* score = dict_.find(member);
  if (score == nullptr) return false;
  // Copy the score before erasing: the dict entry owns it.
  const double stored = *score;
  skiplist_.remove(stored, member);
  dict_.erase(member);
  return true;
}

int64_t ZSet::rank(std::string_view member, bool reverse) const {
  const double* score = dict_.find(member);
  if (score == nullptr) return -1;
  const int64_t ascending = skiplist_.rank_of(*score, member);
  if (ascending < 0) return -1;
  return reverse ? static_cast<int64_t>(skiplist_.size()) - 1 - ascending : ascending;
}

}  // namespace credis
