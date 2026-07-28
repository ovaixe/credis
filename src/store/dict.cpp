#include "store/dict.h"

namespace credis {
namespace {

uint64_t g_hash_seed = 0x9e3779b97f4a7c15ULL;

uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

}  // namespace

void set_hash_seed(uint64_t seed) { g_hash_seed = seed; }

// xxHash64-style mixing: fast on short keys and well distributed, which is all
// the dict needs. Seeded at startup so bucket placement is not attacker-known.
uint64_t hash_bytes(const void* data, size_t len) {
  constexpr uint64_t kPrime1 = 0x9e3779b185ebca87ULL;
  constexpr uint64_t kPrime2 = 0xc2b2ae3d27d4eb4fULL;
  constexpr uint64_t kPrime3 = 0x165667b19e3779f9ULL;

  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = g_hash_seed + kPrime3 + len;

  while (len >= 8) {
    uint64_t k = 0;
    std::memcpy(&k, p, 8);
    k *= kPrime2;
    k = rotl(k, 31);
    k *= kPrime1;
    h ^= k;
    h = rotl(h, 27) * kPrime1 + kPrime3;
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    uint32_t k32 = 0;
    std::memcpy(&k32, p, 4);
    h ^= static_cast<uint64_t>(k32) * kPrime1;
    h = rotl(h, 23) * kPrime2 + kPrime3;
    p += 4;
    len -= 4;
  }
  while (len > 0) {
    h ^= static_cast<uint64_t>(*p) * kPrime3;
    h = rotl(h, 11) * kPrime1;
    ++p;
    --len;
  }

  h ^= h >> 33;
  h *= kPrime2;
  h ^= h >> 29;
  h *= kPrime3;
  h ^= h >> 32;
  return h;
}

}  // namespace credis
