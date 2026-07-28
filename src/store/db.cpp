#include "store/db.h"

#include <string>

#include "util/time.h"

namespace credis {

Object* Db::lookup_read(std::string_view key) {
  expire_if_needed(key);
  return dict_.find(key);
}

Object* Db::lookup_write(std::string_view key) {
  expire_if_needed(key);
  return dict_.find(key);
}

Object* Db::add(std::string_view key, Object value) {
  return dict_.insert(key, std::move(value));
}

Object* Db::set(std::string_view key, Object value, bool keep_ttl) {
  Object* existing = dict_.find(key);
  if (existing != nullptr) {
    *existing = std::move(value);
    if (!keep_ttl) remove_expire(key);
    return existing;
  }
  // A brand new key never inherits a TTL.
  if (!keep_ttl) remove_expire(key);
  return dict_.insert(key, std::move(value));
}

bool Db::erase(std::string_view key) {
  expires_.erase(key);
  return dict_.erase(key);
}

void Db::clear() {
  dict_.clear();
  expires_.clear();
}

void Db::swap_contents(Db& other) {
  dict_.swap(other.dict_);
  expires_.swap(other.expires_);
}

void Db::set_expire(std::string_view key, int64_t when_ms) { expires_.set(key, when_ms); }

bool Db::remove_expire(std::string_view key) { return expires_.erase(key); }

int64_t Db::get_expire(std::string_view key) const {
  const int64_t* when = expires_.find(key);
  return when != nullptr ? *when : kNoExpire;
}

bool Db::expire_if_needed(std::string_view key) {
  const int64_t* when = expires_.find(key);
  if (when == nullptr) return false;
  if (*when > mstime()) return false;

  // Copy the key: erasing frees the dict entry that `key` may point into.
  const std::string owned(key);
  expires_.erase(owned);
  dict_.erase(owned);
  return true;
}

int Db::active_expire_cycle(int64_t deadline_us) {
  if (expires_.empty()) return 0;

  int deleted = 0;
  const int64_t now = mstime();

  // Keep sampling while a sample comes back mostly expired: that means there is
  // probably much more to collect, so it is worth another round.
  for (;;) {
    const size_t remaining = expires_.size();
    if (remaining == 0) break;

    const int sample_size =
        static_cast<int>(remaining < kExpireSample ? remaining : kExpireSample);
    int expired_in_sample = 0;

    for (int i = 0; i < sample_size; ++i) {
      auto* entry = expires_.random_entry();
      if (entry == nullptr) break;
      if (entry->value > now) continue;

      // entry is freed by the erase below, so take a copy of the key first.
      const std::string key = entry->key;
      expires_.erase(key);
      dict_.erase(key);
      ++expired_in_sample;
      ++deleted;
    }

    // Fewer than a quarter expired: the keyspace is mostly fresh, stop here.
    if (expired_in_sample * 4 < sample_size) break;
    if (ustime() > deadline_us) break;
  }
  return deleted;
}

}  // namespace credis
