#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace credis {

// A growable byte buffer with independent read and write cursors.
//
//   +-------------------+------------------+------------------+
//   | already consumed  |  readable bytes  |  writable bytes  |
//   +-------------------+------------------+------------------+
//   0              read_idx_          write_idx_          size()
//
// Consumed space at the front is reclaimed by a memmove when the buffer needs
// room, so a connection issuing a long stream of small commands does not grow
// without bound.
class Buffer {
 public:
  static constexpr size_t kInitialSize = 1024;
  static constexpr size_t kNpos = static_cast<size_t>(-1);

  explicit Buffer(size_t initial_size = kInitialSize) : buf_(initial_size) {}

  size_t readable_bytes() const { return write_idx_ - read_idx_; }
  size_t writable_bytes() const { return buf_.size() - write_idx_; }
  bool empty() const { return readable_bytes() == 0; }
  size_t capacity() const { return buf_.size(); }

  const char* peek() const { return buf_.data() + read_idx_; }
  std::string_view view() const { return {peek(), readable_bytes()}; }

  // Marks `n` bytes at the front as consumed. Resets both cursors when the
  // buffer drains, which is the common case and keeps peek() cache-friendly.
  void consume(size_t n);
  void consume_all() { read_idx_ = write_idx_ = 0; }

  void ensure_writable(size_t n);
  char* begin_write() { return buf_.data() + write_idx_; }
  void has_written(size_t n) { write_idx_ += n; }

  void append(const char* data, size_t len);
  void append(std::string_view data) { append(data.data(), data.size()); }
  void append(char c) { append(&c, 1); }

  // Offset of the next "\r\n" at or after `from` (relative to peek()), or kNpos.
  size_t find_crlf(size_t from = 0) const;
  // Offset of the next '\n' at or after `from` (relative to peek()), or kNpos.
  size_t find_lf(size_t from = 0) const;

  // Reads once from `fd` using a stack-allocated scatter buffer, so a large
  // burst still costs a single read() syscall without over-allocating up front.
  // Returns what read(2) returned; sets *saved_errno on failure.
  ssize_t read_fd(int fd, int* saved_errno);

  // Releases memory when the buffer grew past `threshold` but is now mostly idle.
  void shrink_if_oversized(size_t threshold);

 private:
  void make_space(size_t n);

  std::vector<char> buf_;
  size_t read_idx_ = 0;
  size_t write_idx_ = 0;
};

}  // namespace credis
