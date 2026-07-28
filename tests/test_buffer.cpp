#include <fcntl.h>
#include <unistd.h>

#include <string>

#include "harness.h"
#include "net/buffer.h"

using namespace credis;

TEST(Buffer, AppendAndConsume) {
  Buffer buf;
  CHECK(buf.empty());
  buf.append("hello");
  CHECK_EQ(buf.readable_bytes(), 5u);
  CHECK_BYTES(buf.view(), "hello");

  buf.consume(2);
  CHECK_BYTES(buf.view(), "llo");
  buf.consume(3);
  CHECK(buf.empty());
  CHECK_EQ(buf.readable_bytes(), 0u);
}

TEST(Buffer, GrowsBeyondInitialCapacity) {
  Buffer buf(16);
  const std::string big(10000, 'x');
  buf.append(big);
  CHECK_EQ(buf.readable_bytes(), big.size());
  CHECK_BYTES(buf.view(), big);
}

TEST(Buffer, ReclaimsConsumedSpaceInsteadOfGrowing) {
  Buffer buf(64);
  // Repeatedly filling and draining must not grow the buffer without bound: the
  // hole at the front is compacted away instead.
  for (int i = 0; i < 1000; ++i) {
    buf.append(std::string(32, 'a'));
    buf.consume(32);
  }
  CHECK_LE(buf.capacity(), 256u);
}

TEST(Buffer, FindCrlf) {
  Buffer buf;
  buf.append("*1\r\n$4\r\nPING\r\n");
  CHECK_EQ(buf.find_crlf(), 2u);
  CHECK_EQ(buf.find_crlf(3), 6u);
  CHECK_EQ(buf.find_crlf(100), Buffer::kNpos);

  Buffer partial;
  partial.append("abc\r");
  CHECK_EQ(partial.find_crlf(), Buffer::kNpos);
  partial.append("\n");
  CHECK_EQ(partial.find_crlf(), 3u);
}

TEST(Buffer, FindLf) {
  Buffer buf;
  buf.append("PING\nECHO\n");
  CHECK_EQ(buf.find_lf(), 4u);
  CHECK_EQ(buf.find_lf(5), 9u);
  CHECK_EQ(buf.find_lf(10), Buffer::kNpos);
}

TEST(Buffer, ReadFromFdHandlesLargePayloadInOneCall) {
  int fds[2];
  CHECK_EQ(::pipe(fds), 0);

  // Larger than the buffer's initial capacity so the scatter read path is used.
  const std::string payload(200000, 'z');
  Buffer buf(1024);

  // Write from a child-free loop: fill the pipe in chunks while draining.
  size_t written = 0;
  size_t total_read = 0;
  ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
  ::fcntl(fds[1], F_SETFL, O_NONBLOCK);

  while (total_read < payload.size()) {
    if (written < payload.size()) {
      const ssize_t n = ::write(fds[1], payload.data() + written, payload.size() - written);
      if (n > 0) written += static_cast<size_t>(n);
    }
    int err = 0;
    const ssize_t n = buf.read_fd(fds[0], &err);
    if (n > 0) total_read += static_cast<size_t>(n);
    if (n < 0 && err != EAGAIN && err != EWOULDBLOCK) break;
  }

  CHECK_EQ(buf.readable_bytes(), payload.size());
  CHECK_BYTES(buf.view(), payload);
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(Buffer, ShrinkReleasesMemoryButKeepsData) {
  Buffer buf(1024);
  buf.append(std::string(100000, 'q'));
  buf.consume(99990);
  buf.shrink_if_oversized(4096);
  CHECK_LE(buf.capacity(), 4096u);
  CHECK_BYTES(buf.view(), std::string(10, 'q'));
}
