#include "util/log.h"

#include <unistd.h>

#include <cstdio>
#include <ctime>

#include "util/time.h"

namespace credis {
namespace {

LogLevel g_level = LogLevel::Notice;

char level_char(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return '.';
    case LogLevel::Verbose: return '-';
    case LogLevel::Notice: return '*';
    case LogLevel::Warning: return '#';
  }
  return '*';
}

}  // namespace

void set_log_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

void log_write(LogLevel level, std::string_view message) {
  if (level < g_level) return;

  const int64_t now = mstime();
  const time_t secs = static_cast<time_t>(now / 1000);
  tm tm_buf{};
  localtime_r(&secs, &tm_buf);

  char timestamp[64];
  size_t n = strftime(timestamp, sizeof(timestamp), "%d %b %Y %H:%M:%S", &tm_buf);
  snprintf(timestamp + n, sizeof(timestamp) - n, ".%03d", static_cast<int>(now % 1000));

  fprintf(stderr, "%d:M %s %c %.*s\n", static_cast<int>(getpid()), timestamp, level_char(level),
          static_cast<int>(message.size()), message.data());
  fflush(stderr);
}

}  // namespace credis
