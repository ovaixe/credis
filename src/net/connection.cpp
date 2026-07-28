#include "net/connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <format>

#include "commands/table.h"
#include "server.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/time.h"

namespace credis {

Connection::Connection(Server& server, EventLoop& loop, int fd, std::string peer, uint64_t id)
    : server_(server),
      loop_(loop),
      fd_(fd),
      id_(id),
      peer_(std::move(peer)),
      writer_(out_buf_, RespProtocol::Resp2) {
  created_ms_ = mstime();
  last_interaction_ms_ = created_ms_;
  loop_.add(fd_, EventLoop::kRead, this);
}

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

Db& Connection::db() { return server_.db(db_index_); }

void Connection::reset_state() {
  reader_.reset();
  argv_.clear();
  db_index_ = 0;
  name_.clear();
  writer_.set_protocol(RespProtocol::Resp2);
}

void Connection::handle_read() {
  int saved_errno = 0;
  const ssize_t n = in_buf_.read_fd(fd_, &saved_errno);

  if (n == 0) {  // orderly shutdown by the peer
    close();
    return;
  }
  if (n < 0) {
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK || saved_errno == EINTR) return;
    CREDIS_LOG_VERBOSE("read error on client {}: {}", id_, strerror(saved_errno));
    close();
    return;
  }

  server_.stats().total_net_input_bytes += static_cast<uint64_t>(n);
  last_interaction_ms_ = mstime();
  process_input();
}

void Connection::handle_write() { flush_output(); }

void Connection::handle_error() { close(); }

void Connection::process_input() {
  // Drain every complete command sitting in the buffer. Pipelined requests are
  // therefore handled without another trip through epoll.
  for (;;) {
    if (closing_ || close_after_reply_) break;

    const RespReader::Result result = reader_.parse(in_buf_);
    if (result.status == RespReader::Status::Incomplete) break;

    if (result.status == RespReader::Status::ProtocolError) {
      writer_.error(result.error);
      // A protocol error desynchronizes the stream; the connection cannot
      // continue, so reply and hang up.
      close_after_reply_ = true;
      break;
    }

    argv_ = std::move(reader_.mutable_argv());
    reader_.mutable_argv().clear();
    if (!argv_.empty()) execute_command();
  }

  // Reclaim a query buffer that a large request grew.
  if (in_buf_.empty()) in_buf_.shrink_if_oversized(kBufferShrinkThreshold);

  flush_output();
}

void Connection::reply_unknown_command() {
  // Matches Redis's wording, including the trailing ", " after each argument.
  std::string args;
  for (size_t i = 1; i < argv_.size() && args.size() < 128; ++i) {
    args += '\'';
    args.append(argv_[i], 0, 128 - std::min<size_t>(args.size(), 128));
    args += "', ";
  }
  writer_.error(
      std::format("ERR unknown command '{}', with args beginning with: {}", argv_[0], args));
}

void Connection::execute_command() {
  const Command* command = lookup_command(argv_[0]);
  if (command == nullptr) {
    reply_unknown_command();
    return;
  }
  if (!arity_ok(*command, argv_.size())) {
    writer_.error(std::format("ERR wrong number of arguments for '{}' command",
                              to_lower(argv_[0])));
    return;
  }

  last_command_ = command->name.data();
  command->proc(*this);
  ++server_.stats().commands_processed;

  if (command->flags & kCmdWrite) {
    server_.propagate(argv_, db_index_);
  }
}

void Connection::flush_output() {
  if (closing_) return;

  if (out_buf_.readable_bytes() > kMaxOutputBuffer) {
    CREDIS_LOG_WARNING("client {} scheduled to be closed ASAP: output buffer limit reached", id_);
    close();
    return;
  }

  while (!out_buf_.empty()) {
    // MSG_NOSIGNAL: a write to a closed peer must return EPIPE, not raise a
    // signal that would kill the server.
    const ssize_t n =
        ::send(fd_, out_buf_.peek(), out_buf_.readable_bytes(), MSG_NOSIGNAL);
    if (n > 0) {
      out_buf_.consume(static_cast<size_t>(n));
      server_.stats().total_net_output_bytes += static_cast<uint64_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    CREDIS_LOG_VERBOSE("write error on client {}: {}", id_, strerror(errno));
    close();
    return;
  }

  if (!out_buf_.empty()) {
    // The socket is full; finish in handle_write() once it drains.
    if (!write_armed_) {
      loop_.update(fd_, EventLoop::kRead | EventLoop::kWrite, this);
      write_armed_ = true;
    }
    return;
  }

  if (write_armed_) {
    loop_.update(fd_, EventLoop::kRead, this);
    write_armed_ = false;
  }
  out_buf_.shrink_if_oversized(kBufferShrinkThreshold);

  if (close_after_reply_) close();
}

void Connection::close() {
  if (closing_) return;
  closing_ = true;

  // Unregister before handing ownership over, so no further events are
  // dispatched to this object. The fd stays open until the destructor runs, at
  // the end of the current event batch, which keeps it from being reused early.
  loop_.remove(fd_, this);
  server_.close_client(this);
}

}  // namespace credis
