#include <sys/eventfd.h>
#include <unistd.h>

#include "harness.h"
#include "net/event_loop.h"
#include "util/time.h"

using namespace credis;

namespace {

// Counts readiness callbacks and stops the loop after a target number of them.
struct CountingHandler : EventHandler {
  EventLoop* loop = nullptr;
  int fd = -1;
  int reads = 0;
  int stop_after = 1;

  void handle_read() override {
    uint64_t value = 0;
    ssize_t n = ::read(fd, &value, sizeof(value));
    (void)n;
    ++reads;
    if (reads >= stop_after) loop->stop();
  }
};

}  // namespace

TEST(EventLoop, DispatchesReadReadiness) {
  EventLoop loop;
  const int fd = ::eventfd(0, EFD_NONBLOCK);
  CHECK(fd >= 0);

  CountingHandler handler;
  handler.loop = &loop;
  handler.fd = fd;
  CHECK(loop.add(fd, EventLoop::kRead, &handler));

  const uint64_t one = 1;
  CHECK_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));

  loop.run();
  CHECK_EQ(handler.reads, 1);

  loop.remove(fd, &handler);
  ::close(fd);
}

// Regression: the loop used to compute the epoll timeout from the timer heap but
// never fire the timers, so serverCron — and with it active expiry — never ran.
TEST(EventLoop, FiresPeriodicTimers) {
  EventLoop loop;
  int ticks = 0;
  loop.add_periodic_timer(10, [&] {
    if (++ticks >= 3) loop.stop();
  });

  const int64_t started = monotonic_ms();
  loop.run();

  CHECK_GE(ticks, 3);
  // Three 10ms ticks cannot have completed instantly.
  CHECK_GE(monotonic_ms() - started, 25);
}

TEST(EventLoop, FiresTimersWithNoRegisteredFds) {
  EventLoop loop;
  int ticks = 0;
  loop.add_periodic_timer(5, [&] {
    ++ticks;
    loop.stop();
  });
  loop.run();
  CHECK_EQ(ticks, 1);
}

TEST(EventLoop, RunsDeferredCallbacksAfterDispatch) {
  EventLoop loop;
  const int fd = ::eventfd(0, EFD_NONBLOCK);
  CHECK(fd >= 0);

  std::vector<std::string> order;
  struct DeferringHandler : EventHandler {
    EventLoop* loop = nullptr;
    int fd = -1;
    std::vector<std::string>* order = nullptr;

    void handle_read() override {
      uint64_t value = 0;
      ssize_t n = ::read(fd, &value, sizeof(value));
      (void)n;
      order->push_back("handler");
      loop->defer([this] { order->push_back("deferred"); });
      loop->stop();
    }
  } handler;
  handler.loop = &loop;
  handler.fd = fd;
  handler.order = &order;

  CHECK(loop.add(fd, EventLoop::kRead, &handler));
  const uint64_t one = 1;
  CHECK_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));

  loop.run();

  // The deferred callback must run after the handler that queued it.
  CHECK_EQ(order.size(), 2u);
  CHECK_BYTES(order[0], "handler");
  CHECK_BYTES(order[1], "deferred");

  loop.remove(fd, &handler);
  ::close(fd);
}

TEST(EventLoop, StopIsIdempotentAndWakesTheLoop) {
  EventLoop loop;
  loop.stop();
  // A loop stopped before it started must not block on epoll_wait.
  loop.run();
  CHECK_FALSE(loop.running());
}
