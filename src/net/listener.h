#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "net/event_loop.h"

namespace credis {

// Sets O_NONBLOCK and TCP_NODELAY on an accepted socket. Exposed because the
// listener applies it to every connection it hands out.
bool set_nonblocking(int fd);
bool set_tcp_nodelay(int fd);

// Listening socket. Accepts in a loop until EAGAIN so a burst of connections is
// drained in one readiness notification.
class Listener : public EventHandler {
 public:
  // Called once per accepted connection with an already non-blocking fd and a
  // printable "addr:port" description of the peer.
  using AcceptCallback = std::function<void(int fd, std::string peer)>;

  Listener(EventLoop& loop, AcceptCallback cb);
  ~Listener() override;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Binds and listens. On failure returns false and fills *error.
  bool start(const std::string& bind_addr, uint16_t port, int backlog, std::string* error);

  void handle_read() override;

  int fd() const { return fd_; }

 private:
  EventLoop& loop_;
  AcceptCallback cb_;
  int fd_ = -1;
};

}  // namespace credis
