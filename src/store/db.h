#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "store/dict.h"
#include "store/object.h"

namespace credis {

// One logical database (SELECT picks between them).
//
// Expiration follows Redis's two-pronged strategy:
//
//   * lazily, on every lookup — an expired key is deleted the moment anyone
//     touches it, so it can never be observed;
//   * actively, from serverCron — random sampling deletes keys nobody touches,
//     so memory is reclaimed even for untouched keys.
class Db {
 public:
  explicit Db(int index) : index_(index) {}

  int index() const { return index_; }

  // Number of live keys. Keys that have passed their TTL but not yet been
  // collected are still counted, exactly as in Redis.
  size_t size() const { return dict_.size(); }

  // --- lookups (expire the key first if its TTL has passed) ---
  Object* lookup_read(std::string_view key);
  Object* lookup_write(std::string_view key);

  // --- mutation ---
  // Adds a key that must not already exist. Returns the stored object.
  Object* add(std::string_view key, Object value);
  // Adds or replaces. Any existing TTL is discarded unless keep_ttl is set,
  // which is what SET ... KEEPTTL needs.
  Object* set(std::string_view key, Object value, bool keep_ttl = false);
  bool erase(std::string_view key);
  void clear();
  // Exchanges the contents of two databases without changing their indexes,
  // which is what SWAPDB needs.
  void swap_contents(Db& other);

  // --- expiration ---
  static constexpr int64_t kNoExpire = -1;

  void set_expire(std::string_view key, int64_t when_ms);
  bool remove_expire(std::string_view key);
  // Absolute expiry time in ms, or kNoExpire.
  int64_t get_expire(std::string_view key) const;

  // Deletes `key` if its TTL has passed. Returns true if it was removed.
  bool expire_if_needed(std::string_view key);

  // Redis's adaptive sampling: repeatedly sample up to kExpireSample keys with a
  // TTL and delete the expired ones, continuing while more than 25% of a sample
  // was expired. Stops at `deadline_us` so a cron tick cannot stall the loop.
  // Returns the number of keys deleted.
  int active_expire_cycle(int64_t deadline_us);

  Dict<Object>& dict() { return dict_; }
  const Dict<Object>& dict() const { return dict_; }
  Dict<int64_t>& expires() { return expires_; }
  const Dict<int64_t>& expires() const { return expires_; }

 private:
  static constexpr int kExpireSample = 20;

  int index_;
  Dict<Object> dict_;
  // Only keys that actually have a TTL appear here, so sampling is cheap even
  // when most of the keyspace is persistent.
  Dict<int64_t> expires_;
};

}  // namespace credis
