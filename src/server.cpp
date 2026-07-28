#include "server.h"

#include <unistd.h>

#include <algorithm>

#include "util/strings.h"
#include "util/time.h"

namespace credis {

Server::Server(ServerConfig config) : config_(std::move(config)) {
  dbs_.reserve(static_cast<size_t>(config_.databases));
  for (int i = 0; i < config_.databases; ++i) dbs_.emplace_back(i);
}

Server::~Server() = default;

bool Server::start(std::string* error) {
  start_time_ms_ = mstime();

  listener_ = std::make_unique<Listener>(
      loop_, [this](int fd, std::string peer) { on_accept(fd, std::move(peer)); });
  if (!listener_->start(config_.bind_addr, config_.port, config_.tcp_backlog, error)) {
    return false;
  }

  const int64_t interval_ms = std::max(1, 1000 / std::max(1, config_.hz));
  loop_.add_periodic_timer(interval_ms, [this] { server_cron(); });

  CREDIS_LOG_NOTICE("credis started, version 0.1.0, pid {}", static_cast<int>(getpid()));
  CREDIS_LOG_NOTICE("Ready to accept connections tcp, port {}", config_.port);
  return true;
}

void Server::run() { loop_.run(); }

void Server::stop() {
  CREDIS_LOG_NOTICE("Received shutdown request, exiting");
  loop_.stop();
}

void Server::on_accept(int fd, std::string peer) {
  ++stats_.connections_received;

  if (clients_.size() >= config_.maxclients) {
    ++stats_.rejected_connections;
    // Report the reason before hanging up, exactly as Redis does.
    static constexpr std::string_view kMessage =
        "-ERR max number of clients reached\r\n";
    ssize_t ignored = ::write(fd, kMessage.data(), kMessage.size());
    (void)ignored;
    ::close(fd);
    return;
  }

  const uint64_t id = next_client_id_++;
  auto connection = std::make_unique<Connection>(*this, loop_, fd, std::move(peer), id);
  clients_.emplace(id, std::move(connection));
}

Connection* Server::find_client(uint64_t id) {
  auto it = clients_.find(id);
  return it == clients_.end() ? nullptr : it->second.get();
}

void Server::close_client(Connection* connection) {
  const uint64_t id = connection->id();
  auto it = clients_.find(id);
  if (it == clients_.end()) return;

  // Hand ownership to the loop so the object outlives the current event batch;
  // another handler in the same batch may still hold a pointer to it.
  auto owned = std::move(it->second);
  clients_.erase(it);
  loop_.defer([held = std::shared_ptr<Connection>(std::move(owned))]() mutable { held.reset(); });
}

void Server::swap_dbs(int a, int b) {
  if (a == b) return;
  // Swap contents only: each Db keeps its own index, and clients that already
  // selected a database stay pointed at the same slot.
  dbs_[static_cast<size_t>(a)].swap_contents(dbs_[static_cast<size_t>(b)]);
}

void Server::propagate(const std::vector<std::string>& argv, int db_index) {
  // Intentionally empty. The call sites and the kCmdWrite flag exist so AOF and
  // replication can be added without touching every command implementation.
  (void)argv;
  (void)db_index;
}

void Server::server_cron() {
  if (!config_.active_expire) return;

  // Give the expiry cycle a slice of the tick, never the whole thing: commands
  // must stay responsive. Redis uses 25% of a cron period.
  const int64_t interval_us = (1000 * 1000) / std::max(1, config_.hz);
  const int64_t deadline_us = ustime() + interval_us / 4;

  // Visit a few databases per tick, continuing where the last tick stopped.
  const int to_visit = std::min(db_count(), 16);
  for (int i = 0; i < to_visit; ++i) {
    Db& database = dbs_[static_cast<size_t>(cron_db_cursor_)];
    cron_db_cursor_ = (cron_db_cursor_ + 1) % db_count();

    const int deleted = database.active_expire_cycle(deadline_us);
    stats_.expired_keys += static_cast<uint64_t>(deleted);
    if (ustime() > deadline_us) break;
  }
}

}  // namespace credis
