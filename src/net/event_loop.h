#pragma once

#include <sys/epoll.h>

#include <csignal>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

namespace credis {

// Anything the loop can dispatch readiness to.
class EventHandler {
 public:
  virtual ~EventHandler() = default;
  virtual void handle_read() {}
  virtual void handle_write() {}
  // Peer hangup or socket error. Handlers normally close themselves here.
  virtual void handle_error() {}
};

// Level-triggered epoll reactor. Level-triggered (rather than edge-triggered) is
// deliberate: a command handler may stop reading mid-buffer, and level-triggered
// readiness means the loop simply reports the fd again instead of stalling.
class EventLoop {
 public:
  using TimerCallback = std::function<void()>;
  using DeferredCallback = std::function<void()>;

  static constexpr uint32_t kNone = 0;
  static constexpr uint32_t kRead = 1u << 0;
  static constexpr uint32_t kWrite = 1u << 1;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool add(int fd, uint32_t events, EventHandler* handler);
  bool update(int fd, uint32_t events, EventHandler* handler);
  // `handler` is used to suppress already-dispatched events for a handler that
  // is being torn down mid-batch; pass it whenever one is associated with `fd`.
  void remove(int fd, EventHandler* handler = nullptr);

  // Runs `cb` every `interval_ms`. Timers are few (currently just serverCron), so
  // they live in a flat vector scanned each tick.
  void add_periodic_timer(int64_t interval_ms, TimerCallback cb);

  // Runs `cb` once, after the current batch of events has been fully dispatched.
  // This is how connections are destroyed safely: freeing a handler while its fd
  // still has pending events in the same batch would dangle.
  void defer(DeferredCallback cb) { deferred_.push_back(std::move(cb)); }

  void run();
  // Requests shutdown. Safe to call from a signal handler, from inside a
  // handler, or before run() has started — the request is sticky, so a signal
  // arriving between start() and run() cannot be lost.
  void stop();

  // Async-signal-safe: writes to an eventfd so a blocked epoll_wait returns.
  void wakeup();

  bool running() const { return running_; }
  bool stop_requested() const { return stop_requested_ != 0; }

 private:
  int64_t next_timeout_ms() const;
  void fire_due_timers();
  void run_deferred();

  struct Timer {
    int64_t next_ms;
    int64_t interval_ms;
    TimerCallback cb;
  };

  int epoll_fd_ = -1;
  int wakeup_fd_ = -1;
  bool running_ = false;
  // Written by stop(), possibly from a signal handler, and never cleared.
  volatile sig_atomic_t stop_requested_ = 0;
  std::vector<epoll_event> active_;
  std::vector<Timer> timers_;
  std::vector<DeferredCallback> deferred_;
  // Handlers unregistered during the current dispatch; their remaining events in
  // this batch must be skipped because the object is queued for destruction.
  std::unordered_set<EventHandler*> removed_during_dispatch_;
  bool dispatching_ = false;
};

}  // namespace credis
