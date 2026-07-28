#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/buffer.h"
#include "net/event_loop.h"
#include "resp/reader.h"
#include "resp/writer.h"

namespace credis {

class Db;
class Server;

// One connected client: its socket, its parse state, and the per-connection
// settings commands can change (selected database, protocol version, name).
//
// The read path is: read() -> parse as many complete commands as the buffer
// holds -> execute each, appending replies -> write out what we can. Pipelined
// commands therefore cost one read and one write regardless of how many arrived.
class Connection : public EventHandler {
 public:
  // Beyond this many buffered reply bytes the client is not draining fast enough
  // and gets dropped, rather than letting the server run out of memory.
  static constexpr size_t kMaxOutputBuffer = 512u * 1024 * 1024;
  // Read/reply buffers larger than this are released once the connection idles.
  static constexpr size_t kBufferShrinkThreshold = 64 * 1024;

  Connection(Server& server, EventLoop& loop, int fd, std::string peer, uint64_t id);
  ~Connection() override;

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void handle_read() override;
  void handle_write() override;
  void handle_error() override;

  // --- identity ---
  uint64_t id() const { return id_; }
  int fd() const { return fd_; }
  const std::string& peer() const { return peer_; }
  const std::string& name() const { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  int64_t created_ms() const { return created_ms_; }
  int64_t last_interaction_ms() const { return last_interaction_ms_; }
  const char* last_command() const { return last_command_; }

  // --- reply ---
  RespWriter& out() { return writer_; }
  RespProtocol protocol() const { return writer_.protocol(); }
  void set_protocol(RespProtocol protocol) { writer_.set_protocol(protocol); }
  size_t output_bytes() const { return out_buf_.readable_bytes(); }

  // --- current command ---
  const std::vector<std::string>& argv() const { return argv_; }
  size_t argc() const { return argv_.size(); }
  const std::string& arg(size_t i) const { return argv_[i]; }

  // --- state ---
  Server& server() { return server_; }
  Db& db();
  int db_index() const { return db_index_; }
  void select_db(int index) { db_index_ = index; }

  // Finish writing the pending reply, then disconnect (QUIT, SHUTDOWN).
  void close_after_reply() { close_after_reply_ = true; }
  // RESET: back to a freshly-connected state.
  void reset_state();

  // Flushes buffered replies to the socket, arming EPOLLOUT if it cannot all go
  // out at once.
  void flush_output();

 private:
  void process_input();
  void execute_command();
  void reply_unknown_command();
  // Unregisters and queues this connection for destruction after the current
  // event batch. Safe to call from inside a handler.
  void close();

  Server& server_;
  EventLoop& loop_;
  int fd_;
  uint64_t id_;
  std::string peer_;
  std::string name_;

  Buffer in_buf_;
  Buffer out_buf_;
  RespReader reader_;
  RespWriter writer_;
  std::vector<std::string> argv_;

  int db_index_ = 0;
  bool close_after_reply_ = false;
  bool closing_ = false;
  bool write_armed_ = false;

  int64_t created_ms_ = 0;
  int64_t last_interaction_ms_ = 0;
  // Points at the static command-table name, or "NULL" like Redis's CLIENT LIST.
  const char* last_command_ = "NULL";
};

// Command implementations read like Redis's: they take the client they are
// serving and pull arguments off it.
using Client = Connection;

}  // namespace credis
