#include "net/event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#include "util/log.h"
#include "util/time.h"

namespace credis {
namespace {

constexpr int kInitialEventListSize = 64;
constexpr int kMaxEventListSize = 4096;
// Upper bound on how long epoll_wait may block. Keeps shutdown responsive even
// if no timers are registered.
constexpr int kMaxBlockMs = 100;

uint32_t to_epoll_events(uint32_t events) {
  uint32_t out = 0;
  if (events & EventLoop::kRead) out |= EPOLLIN | EPOLLRDHUP;
  if (events & EventLoop::kWrite) out |= EPOLLOUT;
  return out;
}

}  // namespace

EventLoop::EventLoop() : active_(kInitialEventListSize) {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    CREDIS_LOG_WARNING("epoll_create1 failed: {}", strerror(errno));
    return;
  }
  wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeup_fd_ >= 0) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;  // nullptr handler marks the wakeup fd
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
  }
}

EventLoop::~EventLoop() {
  if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

bool EventLoop::add(int fd, uint32_t events, EventHandler* handler) {
  epoll_event ev{};
  ev.events = to_epoll_events(events);
  ev.data.ptr = handler;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    CREDIS_LOG_WARNING("epoll_ctl(ADD, fd={}) failed: {}", fd, strerror(errno));
    return false;
  }
  removed_during_dispatch_.erase(handler);
  return true;
}

bool EventLoop::update(int fd, uint32_t events, EventHandler* handler) {
  epoll_event ev{};
  ev.events = to_epoll_events(events);
  ev.data.ptr = handler;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    CREDIS_LOG_WARNING("epoll_ctl(MOD, fd={}) failed: {}", fd, strerror(errno));
    return false;
  }
  return true;
}

void EventLoop::remove(int fd, EventHandler* handler) {
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  // Record the handler, not the fd: epoll hands us data.ptr, and it is the
  // handler that is about to be destroyed. Destruction is deferred past the end
  // of the batch, so this pointer cannot be recycled before we finish comparing
  // against it.
  if (dispatching_ && handler != nullptr) removed_during_dispatch_.insert(handler);
}

void EventLoop::add_periodic_timer(int64_t interval_ms, TimerCallback cb) {
  if (interval_ms <= 0) interval_ms = 1;
  timers_.push_back({monotonic_ms() + interval_ms, interval_ms, std::move(cb)});
}

int64_t EventLoop::next_timeout_ms() const {
  int64_t timeout = kMaxBlockMs;
  const int64_t now = monotonic_ms();
  for (const Timer& timer : timers_) {
    timeout = std::min(timeout, timer.next_ms - now);
  }
  return std::max<int64_t>(timeout, 0);
}

void EventLoop::fire_due_timers() {
  const int64_t now = monotonic_ms();
  for (Timer& timer : timers_) {
    if (timer.next_ms > now) continue;
    timer.cb();
    // Skip missed ticks rather than firing in a burst if a command ran long.
    timer.next_ms += timer.interval_ms;
    if (timer.next_ms <= now) timer.next_ms = now + timer.interval_ms;
  }
}

void EventLoop::run_deferred() {
  // Callbacks may themselves defer, so drain by swapping rather than iterating.
  while (!deferred_.empty()) {
    std::vector<DeferredCallback> batch;
    batch.swap(deferred_);
    for (auto& cb : batch) cb();
  }
}

void EventLoop::wakeup() {
  if (wakeup_fd_ < 0) return;
  const uint64_t one = 1;
  ssize_t written = ::write(wakeup_fd_, &one, sizeof(one));
  (void)written;  // Best effort; a full counter already means "wake up".
}

void EventLoop::stop() {
  stop_requested_ = 1;
  wakeup();
}

void EventLoop::run() {
  running_ = true;
  while (stop_requested_ == 0) {
    const int timeout = static_cast<int>(next_timeout_ms());
    const int n = ::epoll_wait(epoll_fd_, active_.data(), static_cast<int>(active_.size()), timeout);

    if (n < 0) {
      if (errno == EINTR) continue;
      CREDIS_LOG_WARNING("epoll_wait failed: {}", strerror(errno));
      break;
    }

    dispatching_ = true;
    for (int i = 0; i < n; ++i) {
      const epoll_event& ev = active_[static_cast<size_t>(i)];
      auto* handler = static_cast<EventHandler*>(ev.data.ptr);

      if (handler == nullptr) {  // wakeup eventfd
        uint64_t drain = 0;
        ssize_t got = ::read(wakeup_fd_, &drain, sizeof(drain));
        (void)got;
        continue;
      }

      // A handler dispatched earlier in this batch may have closed this one; its
      // object is queued for destruction, so skip its remaining events.
      if (removed_during_dispatch_.contains(handler)) continue;

      if (ev.events & (EPOLLHUP | EPOLLERR)) {
        handler->handle_error();
        continue;
      }
      if (ev.events & (EPOLLIN | EPOLLRDHUP)) {
        handler->handle_read();
      }
      if (ev.events & EPOLLOUT) {
        handler->handle_write();
      }
    }
    dispatching_ = false;
    removed_during_dispatch_.clear();

    // Timers run after I/O, every iteration — including when epoll_wait returned
    // because its timeout elapsed rather than because an fd became ready.
    fire_due_timers();

    run_deferred();

    // Grow the event list when a batch fills it, so busy servers make fewer
    // epoll_wait round trips.
    if (n == static_cast<int>(active_.size()) && active_.size() < kMaxEventListSize) {
      active_.resize(active_.size() * 2);
    }
  }
  running_ = false;
  run_deferred();
}

}  // namespace credis
