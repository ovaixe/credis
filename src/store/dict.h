#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace credis {

// Process-wide hash seed, randomized at startup so key distribution cannot be
// predicted from outside. Set once by the server before any dict is populated.
void set_hash_seed(uint64_t seed);
uint64_t hash_bytes(const void* data, size_t len);

// A chained hash table modelled on Redis's dict.c.
//
// Two things make this worth writing instead of reaching for std::unordered_map:
//
//  1. Incremental rehashing. Growing keeps both the old and new bucket arrays
//     alive and migrates a few buckets per operation, so no single command ever
//     pays for a full table resize. std::unordered_map rehashes all at once.
//
//  2. Power-of-two bucket counts, which is what makes the reverse-binary SCAN
//     cursor work (see scan()). libstdc++ uses prime bucket counts, so the
//     guarantee SCAN depends on cannot be built on top of it.
//
// Keys are std::string; V is the mapped type (use Empty for a set).
template <typename V>
class Dict {
 public:
  struct Entry {
    std::string key;
    V value;
    Entry* next = nullptr;
  };

  Dict() = default;
  ~Dict() { clear(); }

  Dict(const Dict&) = delete;
  Dict& operator=(const Dict&) = delete;

  Dict(Dict&& other) noexcept { swap(other); }
  Dict& operator=(Dict&& other) noexcept {
    if (this != &other) {
      clear();
      swap(other);
    }
    return *this;
  }

  void swap(Dict& other) noexcept {
    std::swap(tables_, other.tables_);
    std::swap(rehash_idx_, other.rehash_idx_);
  }

  size_t size() const { return tables_[0].used + tables_[1].used; }
  bool empty() const { return size() == 0; }
  bool is_rehashing() const { return rehash_idx_ >= 0; }
  size_t bucket_count() const { return tables_[0].size + tables_[1].size; }

  // --- lookup ---

  V* find(std::string_view key) {
    Entry* entry = find_entry(key);
    return entry ? &entry->value : nullptr;
  }
  const V* find(std::string_view key) const {
    const Entry* entry = const_cast<Dict*>(this)->find_entry(key);
    return entry ? &entry->value : nullptr;
  }
  bool contains(std::string_view key) const { return const_cast<Dict*>(this)->find_entry(key); }

  // --- mutation ---

  // Inserts only if absent. Returns a pointer to the new value, or nullptr if the
  // key was already present.
  template <typename... Args>
  V* insert(std::string_view key, Args&&... args) {
    if (is_rehashing()) rehash_step();
    expand_if_needed();

    const uint64_t hash = hash_bytes(key.data(), key.size());
    // While rehashing, new keys always go into the new table so the old one only
    // ever shrinks.
    Table& table = is_rehashing() ? tables_[1] : tables_[0];
    if (find_in_table(tables_[0], key, hash)) return nullptr;
    if (is_rehashing() && find_in_table(tables_[1], key, hash)) return nullptr;

    const size_t index = hash & table.mask();
    auto* entry = new Entry{std::string(key), V(std::forward<Args>(args)...), table.buckets[index]};
    table.buckets[index] = entry;
    ++table.used;
    return &entry->value;
  }

  // Inserts or overwrites. Returns true when the key was newly added.
  template <typename U>
  bool set(std::string_view key, U&& value) {
    if (V* existing = find(key)) {
      *existing = std::forward<U>(value);
      return false;
    }
    insert(key, std::forward<U>(value));
    return true;
  }

  // Returns a pointer to the value, inserting a default-constructed one if absent.
  V* find_or_insert(std::string_view key, bool* inserted = nullptr) {
    if (V* existing = find(key)) {
      if (inserted) *inserted = false;
      return existing;
    }
    if (inserted) *inserted = true;
    return insert(key);
  }

  bool erase(std::string_view key) {
    if (size() == 0) return false;
    if (is_rehashing()) rehash_step();

    const uint64_t hash = hash_bytes(key.data(), key.size());
    for (int t = 0; t <= (is_rehashing() ? 1 : 0); ++t) {
      Table& table = tables_[t];
      if (table.size == 0) continue;
      const size_t index = hash & table.mask();
      Entry* prev = nullptr;
      for (Entry* entry = table.buckets[index]; entry != nullptr; entry = entry->next) {
        if (entry->key == key) {
          if (prev) {
            prev->next = entry->next;
          } else {
            table.buckets[index] = entry->next;
          }
          delete entry;
          --table.used;
          shrink_if_needed();
          return true;
        }
        prev = entry;
      }
    }
    return false;
  }

  void clear() {
    for (Table& table : tables_) table.destroy();
    rehash_idx_ = -1;
  }

  // --- iteration ---

  // Suspends rehashing. While paused the bucket arrays cannot be reallocated or
  // swapped, which is what makes it safe to walk them while erasing. Redis does
  // the same thing for its "safe iterators".
  class RehashPause {
   public:
    explicit RehashPause(const Dict& dict) : dict_(dict) { ++dict_.rehash_pause_; }
    ~RehashPause() { --dict_.rehash_pause_; }
    RehashPause(const RehashPause&) = delete;
    RehashPause& operator=(const RehashPause&) = delete;

   private:
    const Dict& dict_;
  };

  bool rehash_paused() const { return rehash_pause_ > 0; }

  // Full, non-resumable iteration. Safe to erase the *current* key from within
  // `fn` — rehashing is paused for the duration, so no bucket array moves under
  // the walk. Inserting during iteration is not supported.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    RehashPause guard(*this);
    for (int t = 0; t < 2; ++t) {
      const Table& table = tables_[t];
      for (size_t i = 0; i < table.size; ++i) {
        Entry* entry = table.buckets[i];
        while (entry != nullptr) {
          Entry* next = entry->next;  // fn may erase entry
          fn(entry->key, entry->value);
          entry = next;
        }
      }
    }
  }

  // Resumable iteration for SCAN. Start with cursor 0; iteration is finished when
  // this returns 0.
  //
  // The cursor is a bucket index incremented in *reverse binary* order — the
  // carry propagates from the high bit down instead of the low bit up. That is
  // what lets the table double or halve mid-iteration without losing elements: a
  // bucket that splits in two has both halves ordered after the cursor's current
  // position. The guarantee it buys is Redis's:
  //
  //   * an element present for the whole iteration is returned at least once
  //   * elements may be returned more than once (callers must tolerate dupes)
  //
  // Requires power-of-two bucket counts, which is why this dict exists.
  template <typename Fn>
  uint64_t scan(uint64_t cursor, Fn&& fn) const {
    if (size() == 0) return 0;

    if (!is_rehashing()) {
      const Table& table = tables_[0];
      const uint64_t mask = table.mask();
      emit_bucket(table, cursor & mask, fn);
      return reverse_increment(cursor, mask);
    }

    // While rehashing, visit the same logical bucket in both tables. t0 is
    // always the smaller one.
    const Table* t0 = &tables_[0];
    const Table* t1 = &tables_[1];
    if (t0->size > t1->size) std::swap(t0, t1);

    const uint64_t m0 = t0->mask();
    const uint64_t m1 = t1->mask();

    emit_bucket(*t0, cursor & m0, fn);
    // Every bucket of the larger table that maps onto this small-table bucket.
    uint64_t c = cursor;
    do {
      emit_bucket(*t1, c & m1, fn);
      c = reverse_increment(c, m1);
      // Keep going while the cursor still differs in the bits the small table's
      // mask does not cover — those are exactly the buckets that split.
    } while ((c & (m0 ^ m1)) != 0);

    // Advancing over the large table by the mask difference is equivalent to a
    // single reverse increment in the small table's space.
    return reverse_increment(cursor, m0);
  }

  // Uniformly-ish random entry, used by RANDOMKEY, SPOP and the active expiry
  // sampler. Returns nullptr only when the dict is empty.
  Entry* random_entry() {
    if (size() == 0) return nullptr;
    if (is_rehashing()) rehash_step();

    Entry* entry = nullptr;
    if (is_rehashing()) {
      // Pick across the union of both tables, skipping the already-migrated
      // prefix of table 0.
      const uint64_t total = tables_[0].size + tables_[1].size;
      do {
        uint64_t h = next_random() % (total - static_cast<uint64_t>(rehash_idx_));
        h += static_cast<uint64_t>(rehash_idx_);
        entry = h >= tables_[0].size ? tables_[1].buckets[h - tables_[0].size]
                                     : tables_[0].buckets[h];
      } while (entry == nullptr);
    } else {
      do {
        entry = tables_[0].buckets[next_random() & tables_[0].mask()];
      } while (entry == nullptr);
    }

    // Walk the chain and pick one of its elements uniformly.
    size_t chain_len = 0;
    for (Entry* e = entry; e != nullptr; e = e->next) ++chain_len;
    size_t steps = next_random() % chain_len;
    while (steps--) entry = entry->next;
    return entry;
  }

  // Migrates up to `buckets` non-empty buckets from the old table to the new one.
  // Returns true while rehashing is still in progress.
  bool rehash_step(int buckets = 1) {
    if (!is_rehashing()) return false;
    // Paused: an iteration is walking the bucket arrays and they must not move.
    if (rehash_pause_ > 0) return true;

    // Bound the scan over empty buckets so a sparse table cannot stall the loop.
    int empty_visits = buckets * 10;
    while (buckets-- > 0 && tables_[0].used != 0) {
      while (tables_[0].buckets[static_cast<size_t>(rehash_idx_)] == nullptr) {
        ++rehash_idx_;
        if (--empty_visits == 0) return true;
      }

      Entry* entry = tables_[0].buckets[static_cast<size_t>(rehash_idx_)];
      while (entry != nullptr) {
        Entry* next = entry->next;
        const size_t index = hash_bytes(entry->key.data(), entry->key.size()) & tables_[1].mask();
        entry->next = tables_[1].buckets[index];
        tables_[1].buckets[index] = entry;
        --tables_[0].used;
        ++tables_[1].used;
        entry = next;
      }
      tables_[0].buckets[static_cast<size_t>(rehash_idx_)] = nullptr;
      ++rehash_idx_;
    }

    if (tables_[0].used == 0) {
      tables_[0].destroy();
      tables_[0] = std::move(tables_[1]);
      tables_[1] = Table{};
      rehash_idx_ = -1;
      return false;
    }
    return true;
  }

 private:
  struct Table {
    Entry** buckets = nullptr;
    size_t size = 0;  // always a power of two (or 0)
    size_t used = 0;

    uint64_t mask() const { return size - 1; }

    void alloc(size_t new_size) {
      buckets = new Entry*[new_size];
      std::memset(buckets, 0, sizeof(Entry*) * new_size);
      size = new_size;
      used = 0;
    }

    void destroy() {
      if (buckets == nullptr) {
        size = used = 0;
        return;
      }
      for (size_t i = 0; i < size; ++i) {
        Entry* entry = buckets[i];
        while (entry != nullptr) {
          Entry* next = entry->next;
          delete entry;
          entry = next;
        }
      }
      delete[] buckets;
      buckets = nullptr;
      size = used = 0;
    }

    Table() = default;
    Table(Table&& other) noexcept
        : buckets(other.buckets), size(other.size), used(other.used) {
      other.buckets = nullptr;
      other.size = other.used = 0;
    }
    Table& operator=(Table&& other) noexcept {
      if (this != &other) {
        buckets = other.buckets;
        size = other.size;
        used = other.used;
        other.buckets = nullptr;
        other.size = other.used = 0;
      }
      return *this;
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
  };

  static constexpr size_t kInitialSize = 4;

  Entry* find_entry(std::string_view key) {
    if (size() == 0) return nullptr;
    if (is_rehashing()) rehash_step();

    const uint64_t hash = hash_bytes(key.data(), key.size());
    if (Entry* found = find_in_table(tables_[0], key, hash)) return found;
    if (is_rehashing()) return find_in_table(tables_[1], key, hash);
    return nullptr;
  }

  static Entry* find_in_table(Table& table, std::string_view key, uint64_t hash) {
    if (table.size == 0) return nullptr;
    for (Entry* entry = table.buckets[hash & table.mask()]; entry != nullptr;
         entry = entry->next) {
      if (entry->key == key) return entry;
    }
    return nullptr;
  }

  template <typename Fn>
  static void emit_bucket(const Table& table, uint64_t index, Fn& fn) {
    for (Entry* entry = table.buckets[index]; entry != nullptr; entry = entry->next) {
      fn(entry->key, entry->value);
    }
  }

  // Adds one to `cursor` with the carry running from the high bit downwards.
  static uint64_t reverse_increment(uint64_t cursor, uint64_t mask) {
    cursor |= ~mask;
    cursor = reverse_bits(cursor);
    ++cursor;
    return reverse_bits(cursor);
  }

  static uint64_t reverse_bits(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
    v = ((v >> 2) & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
    v = ((v >> 4) & 0x0f0f0f0f0f0f0f0fULL) | ((v & 0x0f0f0f0f0f0f0f0fULL) << 4);
    v = ((v >> 8) & 0x00ff00ff00ff00ffULL) | ((v & 0x00ff00ff00ff00ffULL) << 8);
    v = ((v >> 16) & 0x0000ffff0000ffffULL) | ((v & 0x0000ffff0000ffffULL) << 16);
    return (v >> 32) | (v << 32);
  }

  void expand_if_needed() {
    if (is_rehashing() || rehash_pause_ > 0) return;
    if (tables_[0].size == 0) {
      tables_[0].alloc(kInitialSize);
      return;
    }
    // Grow at load factor 1, doubling — same trigger Redis uses.
    if (tables_[0].used >= tables_[0].size) {
      start_rehash(next_power_of_two(tables_[0].used * 2));
    }
  }

  void shrink_if_needed() {
    if (is_rehashing() || rehash_pause_ > 0) return;
    if (tables_[0].size <= kInitialSize) return;
    // Below 10% utilization the table is mostly empty buckets; halve it back down.
    if (tables_[0].used * 10 < tables_[0].size) {
      start_rehash(next_power_of_two(tables_[0].used < kInitialSize ? kInitialSize
                                                                   : tables_[0].used));
    }
  }

  void start_rehash(size_t new_size) {
    if (new_size == tables_[0].size) return;
    tables_[1].alloc(new_size);
    rehash_idx_ = 0;
  }

  static size_t next_power_of_two(size_t n) {
    size_t size = kInitialSize;
    while (size < n) size *= 2;
    return size;
  }

  static uint64_t next_random() {
    // xorshift64*: cheap, and sampling quality only needs to be good enough for
    // RANDOMKEY and expiry sampling.
    static thread_local uint64_t state = 0x2545f4914f6cdd1dULL;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dULL;
  }

  Table tables_[2];
  // -1 when not rehashing; otherwise the next bucket of tables_[0] to migrate.
  int64_t rehash_idx_ = -1;
  // Nesting depth of RehashPause guards; mutable so const iteration can pause.
  mutable int rehash_pause_ = 0;
};

// Mapped type for dicts used as plain sets.
struct Empty {};

}  // namespace credis
