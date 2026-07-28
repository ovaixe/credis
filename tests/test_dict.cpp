#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "harness.h"
#include "store/dict.h"
#include "util/strings.h"

using namespace credis;

TEST(Dict, InsertFindErase) {
  Dict<int> d;
  CHECK(d.empty());
  CHECK(d.insert("a", 1) != nullptr);
  CHECK(d.insert("b", 2) != nullptr);
  CHECK_EQ(d.size(), 2u);

  // Duplicate insert is rejected without touching the existing value.
  CHECK(d.insert("a", 99) == nullptr);
  CHECK_EQ(*d.find("a"), 1);

  CHECK_EQ(*d.find("b"), 2);
  CHECK(d.find("missing") == nullptr);
  CHECK(d.contains("a"));

  CHECK(d.erase("a"));
  CHECK_FALSE(d.erase("a"));
  CHECK_EQ(d.size(), 1u);
  CHECK(d.find("a") == nullptr);
}

TEST(Dict, SetOverwrites) {
  Dict<std::string> d;
  CHECK(d.set("k", "v1"));        // newly added
  CHECK_FALSE(d.set("k", "v2"));  // overwritten
  CHECK_BYTES(*d.find("k"), "v2");
  CHECK_EQ(d.size(), 1u);
}

TEST(Dict, FindOrInsert) {
  Dict<int> d;
  bool inserted = false;
  int* v = d.find_or_insert("x", &inserted);
  CHECK(inserted);
  *v = 7;
  int* again = d.find_or_insert("x", &inserted);
  CHECK_FALSE(inserted);
  CHECK_EQ(*again, 7);
}

TEST(Dict, GrowsAndKeepsEveryKey) {
  Dict<int> d;
  constexpr int kCount = 20000;
  for (int i = 0; i < kCount; ++i) {
    CHECK(d.insert("key:" + ll2string(i), i) != nullptr);
  }
  CHECK_EQ(d.size(), static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    const int* v = d.find("key:" + ll2string(i));
    CHECK(v != nullptr);
    CHECK_EQ(*v, i);
  }
  // Bucket count must be a power of two for the SCAN cursor to be valid.
  size_t buckets = d.bucket_count();
  CHECK((buckets & (buckets - 1)) == 0u);
}

TEST(Dict, ShrinksWhenMostlyEmptied) {
  Dict<int> d;
  for (int i = 0; i < 10000; ++i) d.insert("k" + ll2string(i), i);
  const size_t grown = d.bucket_count();

  for (int i = 0; i < 9950; ++i) d.erase("k" + ll2string(i));
  // Force any in-flight incremental rehash to completion.
  while (d.is_rehashing()) d.rehash_step(100);

  CHECK_EQ(d.size(), 50u);
  CHECK_LT(d.bucket_count(), grown);
  for (int i = 9950; i < 10000; ++i) CHECK(d.contains("k" + ll2string(i)));
}

TEST(Dict, ForEachVisitsEverything) {
  Dict<int> d;
  for (int i = 0; i < 500; ++i) d.insert("k" + ll2string(i), i);

  std::unordered_set<std::string> seen;
  d.for_each([&](const std::string& key, int) { seen.insert(key); });
  CHECK_EQ(seen.size(), 500u);
}

TEST(Dict, ScanVisitsEveryKeyExactlyOnceWhenStable) {
  Dict<int> d;
  constexpr int kCount = 5000;
  for (int i = 0; i < kCount; ++i) d.insert("k" + ll2string(i), i);
  while (d.is_rehashing()) d.rehash_step(100);

  std::unordered_map<std::string, int> counts;
  uint64_t cursor = 0;
  int iterations = 0;
  do {
    cursor = d.scan(cursor, [&](const std::string& key, int) { counts[key]++; });
    CHECK_LT(++iterations, 100000);
  } while (cursor != 0);

  CHECK_EQ(counts.size(), static_cast<size_t>(kCount));
  // With no resize in flight, SCAN should not duplicate anything.
  for (const auto& [key, count] : counts) {
    (void)key;
    CHECK_EQ(count, 1);
  }
}

// The guarantee that makes SCAN usable: a key present for the entire iteration
// is returned at least once, even while the table doubles and halves underneath.
TEST(Dict, ScanNeverMissesAStableKeyWhileRehashing) {
  Dict<int> d;
  constexpr int kStable = 2000;
  for (int i = 0; i < kStable; ++i) d.insert("stable:" + ll2string(i), i);

  std::unordered_set<std::string> seen;
  uint64_t cursor = 0;
  int churn = 0;
  int iterations = 0;

  do {
    cursor = d.scan(cursor, [&](const std::string& key, int) { seen.insert(key); });

    // Churn the table mid-iteration so it is almost always rehashing: add a
    // batch of volatile keys, then delete an older batch.
    for (int i = 0; i < 40; ++i) {
      d.insert("volatile:" + ll2string(churn * 40 + i), 0);
    }
    if (churn > 4) {
      for (int i = 0; i < 40; ++i) {
        d.erase("volatile:" + ll2string((churn - 5) * 40 + i));
      }
    }
    ++churn;
    CHECK_LT(++iterations, 200000);
  } while (cursor != 0);

  // Every stable key must have been reported at least once.
  for (int i = 0; i < kStable; ++i) {
    const std::string key = "stable:" + ll2string(i);
    if (!seen.contains(key)) CREDIS_FAIL("SCAN missed stable key " + key);
  }
}

TEST(Dict, ScanTerminatesOnEmptyAndTinyDicts) {
  Dict<int> empty;
  CHECK_EQ(empty.scan(0, [](const std::string&, int) {}), 0u);

  Dict<int> one;
  one.insert("solo", 1);
  int visits = 0;
  uint64_t cursor = 0;
  int iterations = 0;
  do {
    cursor = one.scan(cursor, [&](const std::string&, int) { ++visits; });
    CHECK_LT(++iterations, 1000);
  } while (cursor != 0);
  CHECK_EQ(visits, 1);
}

TEST(Dict, RandomEntrySpreadsAcrossKeys) {
  Dict<int> d;
  for (int i = 0; i < 100; ++i) d.insert("k" + ll2string(i), i);

  std::set<std::string> picked;
  for (int i = 0; i < 2000; ++i) {
    auto* entry = d.random_entry();
    CHECK(entry != nullptr);
    picked.insert(entry->key);
  }
  // A uniform sampler over 100 keys will realistically hit most of them.
  CHECK_GT(picked.size(), 60u);

  Dict<int> empty;
  CHECK(empty.random_entry() == nullptr);
}

TEST(Dict, WorksAsASet) {
  Dict<Empty> members;
  CHECK(members.insert("alpha") != nullptr);
  CHECK(members.insert("alpha") == nullptr);
  CHECK(members.contains("alpha"));
  CHECK_EQ(members.size(), 1u);
  CHECK(members.erase("alpha"));
  CHECK(members.empty());
}

TEST(Dict, MoveTransfersOwnership) {
  Dict<int> a;
  a.insert("k", 5);
  Dict<int> b = std::move(a);
  CHECK_EQ(b.size(), 1u);
  CHECK_EQ(*b.find("k"), 5);

  Dict<int> c;
  c.insert("other", 9);
  c = std::move(b);
  CHECK_EQ(c.size(), 1u);
  CHECK(c.contains("k"));
  CHECK_FALSE(c.contains("other"));
}

TEST(Dict, HandlesBinaryKeysWithEmbeddedNuls) {
  Dict<int> d;
  const std::string a("a\0b", 3);
  const std::string b("a\0c", 3);
  d.insert(a, 1);
  d.insert(b, 2);
  CHECK_EQ(d.size(), 2u);
  CHECK_EQ(*d.find(a), 1);
  CHECK_EQ(*d.find(b), 2);

  const std::string empty_key;
  d.insert(empty_key, 3);
  CHECK_EQ(*d.find(empty_key), 3);
}
