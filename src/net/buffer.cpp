#include "net/buffer.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>

namespace credis {

void Buffer::consume(size_t n) {
  assert(n <= readable_bytes());
  if (n >= readable_bytes()) {
    consume_all();
  } else {
    read_idx_ += n;
  }
}

void Buffer::make_space(size_t n) {
  if (writable_bytes() >= n) return;

  const size_t readable = readable_bytes();
  // Compacting is enough whenever the hole at the front plus the tail can hold
  // the request; only then do we fall back to reallocating.
  if (read_idx_ > 0 && read_idx_ + writable_bytes() >= n) {
    if (readable > 0) {
      memmove(buf_.data(), buf_.data() + read_idx_, readable);
    }
    read_idx_ = 0;
    write_idx_ = readable;
  } else {
    buf_.resize(write_idx_ + n);
  }
}

void Buffer::ensure_writable(size_t n) { make_space(n); }

void Buffer::append(const char* data, size_t len) {
  if (len == 0) return;
  make_space(len);
  memcpy(begin_write(), data, len);
  write_idx_ += len;
}

size_t Buffer::find_crlf(size_t from) const {
  const size_t readable = readable_bytes();
  if (from >= readable || readable - from < 2) return kNpos;
  const char* start = peek() + from;
  const void* found = memmem(start, readable - from, "\r\n", 2);
  if (found == nullptr) return kNpos;
  return static_cast<size_t>(static_cast<const char*>(found) - peek());
}

size_t Buffer::find_lf(size_t from) const {
  const size_t readable = readable_bytes();
  if (from >= readable) return kNpos;
  const char* start = peek() + from;
  const void* found = memchr(start, '\n', readable - from);
  if (found == nullptr) return kNpos;
  return static_cast<size_t>(static_cast<const char*>(found) - peek());
}

ssize_t Buffer::read_fd(int fd, int* saved_errno) {
  // 64 KiB of stack backs the second iovec: one readv() drains a burst even when
  // the buffer itself is small, and only what actually arrived gets appended.
  char extra[65536];
  iovec vec[2];
  const size_t writable = writable_bytes();
  vec[0].iov_base = begin_write();
  vec[0].iov_len = writable;
  vec[1].iov_base = extra;
  vec[1].iov_len = sizeof(extra);

  const int iovcnt = (writable < sizeof(extra)) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);
  if (n < 0) {
    *saved_errno = errno;
  } else if (static_cast<size_t>(n) <= writable) {
    write_idx_ += static_cast<size_t>(n);
  } else {
    write_idx_ = buf_.size();
    append(extra, static_cast<size_t>(n) - writable);
  }
  return n;
}

void Buffer::shrink_if_oversized(size_t threshold) {
  if (buf_.size() <= threshold) return;
  const size_t readable = readable_bytes();
  if (readable > threshold / 2) return;

  if (readable > 0) memmove(buf_.data(), buf_.data() + read_idx_, readable);
  read_idx_ = 0;
  write_idx_ = readable;
  buf_.resize(threshold);
  buf_.shrink_to_fit();
}

}  // namespace credis
