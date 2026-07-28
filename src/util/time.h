#pragma once

#include <cstdint>
#include <ctime>

namespace credis {

// Milliseconds since the Unix epoch. This is the clock all TTLs are stored against.
inline int64_t mstime() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

inline int64_t ustime() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// Monotonic milliseconds, for measuring durations (unaffected by clock jumps).
inline int64_t monotonic_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace credis
