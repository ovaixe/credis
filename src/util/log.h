#pragma once

#include <format>
#include <string_view>

namespace credis {

enum class LogLevel { Debug = 0, Verbose = 1, Notice = 2, Warning = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();

// Writes one line in Redis's log format:
//   <pid>:M 28 Jul 2026 23:40:26.123 * message
void log_write(LogLevel level, std::string_view message);

template <typename... Args>
void log_msg(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  if (level < log_level()) return;
  log_write(level, std::format(fmt, std::forward<Args>(args)...));
}

#define CREDIS_LOG_DEBUG(...) ::credis::log_msg(::credis::LogLevel::Debug, __VA_ARGS__)
#define CREDIS_LOG_VERBOSE(...) ::credis::log_msg(::credis::LogLevel::Verbose, __VA_ARGS__)
#define CREDIS_LOG_NOTICE(...) ::credis::log_msg(::credis::LogLevel::Notice, __VA_ARGS__)
#define CREDIS_LOG_WARNING(...) ::credis::log_msg(::credis::LogLevel::Warning, __VA_ARGS__)

}  // namespace credis
