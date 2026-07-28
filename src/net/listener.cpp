#include "net/listener.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util/log.h"
#include "util/strings.h"

namespace credis {

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_tcp_nodelay(int fd) {
  const int yes = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}

namespace {

std::string describe_peer(const sockaddr_storage& addr) {
  char host[INET6_ADDRSTRLEN] = {0};
  uint16_t port = 0;
  if (addr.ss_family == AF_INET) {
    const auto* in4 = reinterpret_cast<const sockaddr_in*>(&addr);
    ::inet_ntop(AF_INET, &in4->sin_addr, host, sizeof(host));
    port = ntohs(in4->sin_port);
  } else if (addr.ss_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    ::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
    port = ntohs(in6->sin6_port);
  }
  return std::string(host) + ":" + ll2string(port);
}

}  // namespace

Listener::Listener(EventLoop& loop, AcceptCallback cb) : loop_(loop), cb_(std::move(cb)) {}

Listener::~Listener() {
  if (fd_ >= 0) {
    loop_.remove(fd_, this);
    ::close(fd_);
  }
}

bool Listener::start(const std::string& bind_addr, uint16_t port, int backlog, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

  const std::string port_str = ll2string(port);
  const char* node = bind_addr.empty() || bind_addr == "*" ? nullptr : bind_addr.c_str();

  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(node, port_str.c_str(), &hints, &results);
  if (rc != 0) {
    *error = std::string("cannot resolve bind address '") + bind_addr + "': " + gai_strerror(rc);
    return false;
  }

  std::string last_error = "no usable address";
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const int sock = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (sock < 0) {
      last_error = std::string("socket: ") + strerror(errno);
      continue;
    }

    const int yes = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // Without this a wildcard IPv6 bind would also claim the IPv4 port, making a
    // subsequent IPv4 bind fail confusingly.
    if (ai->ai_family == AF_INET6) {
      ::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
    }

    if (::bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
      last_error = std::string("bind: ") + strerror(errno);
      ::close(sock);
      continue;
    }
    if (::listen(sock, backlog) < 0) {
      last_error = std::string("listen: ") + strerror(errno);
      ::close(sock);
      continue;
    }
    if (!set_nonblocking(sock)) {
      last_error = std::string("fcntl: ") + strerror(errno);
      ::close(sock);
      continue;
    }

    fd_ = sock;
    break;
  }
  ::freeaddrinfo(results);

  if (fd_ < 0) {
    *error = last_error;
    return false;
  }
  if (!loop_.add(fd_, EventLoop::kRead, this)) {
    *error = "cannot register listening socket with the event loop";
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}

void Listener::handle_read() {
  // Drain the accept queue; level-triggered epoll would re-notify, but looping
  // here avoids a syscall round trip per pending connection.
  for (;;) {
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    const int conn_fd =
        ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno == EMFILE || errno == ENFILE) {
        // Out of descriptors. Returning leaves the connection queued; the next
        // readiness notification retries once a descriptor is freed.
        CREDIS_LOG_WARNING("accept failed, out of file descriptors: {}", strerror(errno));
        return;
      }
      CREDIS_LOG_WARNING("accept failed: {}", strerror(errno));
      return;
    }
    set_tcp_nodelay(conn_fd);
    cb_(conn_fd, describe_peer(addr));
  }
}

}  // namespace credis
