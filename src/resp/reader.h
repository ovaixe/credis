#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/buffer.h"

namespace credis {

// Protocol limits, matching Redis.
inline constexpr int64_t kMaxMultibulkLen = 1024 * 1024;
inline constexpr int64_t kMaxBulkLen = 512LL * 1024 * 1024;
inline constexpr size_t kMaxInlineLen = 64 * 1024;

// Incremental request parser.
//
// A single command may arrive spread across any number of read() calls, so the
// reader keeps its position between invocations instead of re-scanning the
// buffer: `multibulk_remaining_` and `bulk_len_` survive an Incomplete result.
// Only fully-parsed bytes are consumed from the buffer.
//
// One reader belongs to one connection.
class RespReader {
 public:
  enum class Status {
    Incomplete,     // need more bytes; call again after the next read()
    Ok,             // argv() holds a complete command (possibly empty, e.g. "*0")
    ProtocolError,  // fatal for the connection; error() explains why
  };

  struct Result {
    Status status = Status::Incomplete;
    std::string error;
  };

  // Parses at most one command from `in`.
  Result parse(Buffer& in);

  // Valid after parse() returns Ok. Empty argv means "no command here"
  // (an empty multibulk or a blank inline line) and should simply be skipped.
  const std::vector<std::string>& argv() const { return argv_; }
  std::vector<std::string>& mutable_argv() { return argv_; }

  // Clears partial state; used when a connection is reset (RESET command).
  void reset();

 private:
  Result parse_inline(Buffer& in);
  Result parse_multibulk(Buffer& in);

  std::vector<std::string> argv_;
  // Number of bulk arguments still to be read for the command in progress.
  // Zero means "not currently inside a multibulk request".
  int64_t multibulk_remaining_ = 0;
  // Length of the bulk argument whose header has been read but whose payload has
  // not yet fully arrived. -1 means the next thing expected is a "$<len>" header.
  int64_t bulk_len_ = -1;
};

}  // namespace credis
