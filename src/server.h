#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/connection.h"
#include "net/event_loop.h"
#include "net/listener.h"
#include "store/db.h"
#include "util/log.h"

namespace credis {

struct ServerConfig {
  std::string bind_addr = "127.0.0.1";
  uint16_t port = 6379;
  int databases = 16;
  int tcp_backlog = 511;
  size_t maxclients = 10000;
  LogLevel loglevel = LogLevel::Notice;
  // serverCron frequency in Hz; 10 means a tick every 100ms, as in Redis.
  int hz = 10;
  std::string logfile;
  std::string dir = ".";
  bool active_expire = true;
};

struct ServerStats {
  uint64_t connections_received = 0;
  uint64_t rejected_connections = 0;
  uint64_t commands_processed = 0;
  uint64_t expired_keys = 0;
  uint64_t keyspace_hits = 0;
  uint64_t keyspace_misses = 0;
  uint64_t total_net_input_bytes = 0;
  uint64_t total_net_output_bytes = 0;
};

// Owns the event loop, the listening socket, the databases and every connected
// client. Single-threaded by design: commands run to completion against the
// keyspace with no locking of any kind.
class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the listening socket and registers the cron timer.
  bool start(std::string* error);
  void run();
  void stop();

  Db& db(int index) { return dbs_[static_cast<size_t>(index)]; }
  int db_count() const { return static_cast<int>(dbs_.size()); }
  // Swaps the contents of two databases (SWAPDB).
  void swap_dbs(int a, int b);

  const ServerConfig& config() const { return config_; }
  ServerConfig& mutable_config() { return config_; }
  ServerStats& stats() { return stats_; }
  const ServerStats& stats() const { return stats_; }
  EventLoop& loop() { return loop_; }

  int64_t start_time_ms() const { return start_time_ms_; }
  uint16_t port() const { return config_.port; }
  size_t client_count() const { return clients_.size(); }

  const std::unordered_map<uint64_t, std::unique_ptr<Connection>>& clients() const {
    return clients_;
  }
  Connection* find_client(uint64_t id);

  // Unregisters a client and frees it once the current event batch is done.
  void close_client(Connection* connection);

  // Hook where AOF/replication propagation will go. Called after every command
  // whose table entry carries kCmdWrite and that reported a change.
  void propagate(const std::vector<std::string>& argv, int db_index);

 private:
  void on_accept(int fd, std::string peer);
  void server_cron();

  ServerConfig config_;
  ServerStats stats_;
  EventLoop loop_;
  std::unique_ptr<Listener> listener_;
  std::vector<Db> dbs_;
  std::unordered_map<uint64_t, std::unique_ptr<Connection>> clients_;
  uint64_t next_client_id_ = 1;
  int64_t start_time_ms_ = 0;
  // Rotates across databases so one cron tick does not always scan db 0.
  int cron_db_cursor_ = 0;
};

}  // namespace credis
