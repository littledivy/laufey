// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Stress test for laufey_sync_call.h, the rendezvous behind cef_invoke_sync
// and gtk_invoke_sync. Each keeps the SyncCall in the waiting thread's stack
// frame and lets the UI thread signal it, so the signal must be the UI
// thread's last access: the waiter returns, and the next call reuses the same
// stack address, as soon as the lock is released. Notifying after the unlock
// hung the CEF native e2e on macOS and crashed it.
//
// The calls below go through the same shapes the backends use (a heap
// std::function on a task queue, a captureless callback with a context
// pointer) hundreds of thousands of times from several threads at once.
// Plainly, a regression shows up as a hang (the watchdog fails the test) or a
// crash; under ThreadSanitizer (the `tsan` CI job) as a data race between the
// waiter destroying the condition variable and the late notify, on the first
// call it happens to.
//
// Plain asserts, no framework. Exits non-zero on the first failure. Build
// with `-std=c++17 -pthread -Ibackend-common/include` (plus
// `-fsanitize=thread` for ThreadSanitizer), as the `sync-call` CI job does.

#include "laufey_sync_call.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using laufey_common::SyncCall;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

// A stand-in for a UI thread: runs posted tasks in order on its own thread.
// Tasks are destroyed after they run, on that thread, as GLib's destroy
// notify and CEF's task runner do.
class Executor {
 public:
  Executor() : thread_([this] { Loop(); }) {}
  ~Executor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }

  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  // g_idle_add / CefPostTask(base::BindOnce(fn, ptr)): a captureless
  // callback and a raw context pointer.
  void PostRaw(void (*fn)(void*), void* data) {
    Post([fn, data] { fn(data); });
  }

  bool OnThread() const {
    return std::this_thread::get_id() == thread_.get_id();
  }

 private:
  void Loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stop_ = false;
  std::thread thread_;
};

// A lambda capturing the frame's SyncCall by reference.
void RunSyncLambda(Executor& ex, const std::function<void()>& fn) {
  if (ex.OnThread()) {
    fn();
    return;
  }
  SyncCall call;
  ex.Post([&] {
    fn();
    call.Done();
  });
  call.Wait();
}

// gtk_invoke_sync / cef_invoke_sync's shape: a context struct in the frame
// handed over as a raw pointer.
template <typename F>
void RunSyncRaw(Executor& ex, F&& fn) {
  struct Ctx {
    F* fn;
    SyncCall call;
  };
  Ctx ctx{&fn, {}};
  ex.PostRaw(
      [](void* data) {
        auto* c = static_cast<Ctx*>(data);
        (*c->fn)();
        c->call.Done();
      },
      &ctx);
  ctx.call.Wait();
}

// Fails the test instead of hanging until the CI job times out when a lost
// wakeup leaves a Wait() asleep (how the bug showed on macOS).
class Watchdog {
 public:
  explicit Watchdog(std::chrono::seconds limit)
      : thread_([this, limit] {
          std::unique_lock<std::mutex> lock(mutex_);
          if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
            std::fprintf(stderr,
                         "sync_call_test: no progress after %lld s: a "
                         "SyncCall::Wait() never woke\n",
                         static_cast<long long>(limit.count()));
            std::_Exit(1);
          }
        }) {}
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

// ThreadSanitizer reports a race on the first bad call, but runs ~10x slower.
int Iterations(int plain, int under_tsan) {
  return kThreadSanitizer ? under_tsan : plain;
}

}  // namespace

// One caller, back to back: every call's SyncCall lands at the same stack
// address as the previous one's.
static void TestBackToBack() {
  Executor ex;
  const int n = Iterations(100000, 20000);
  int counter = 0;  // only touched on the executor thread
  for (int i = 0; i < n; ++i)
    RunSyncLambda(ex, [&] { ++counter; });
  // Reading it here is ordered after every task by the rendezvous.
  EXPECT(counter == n);

  int raw = 0;
  for (int i = 0; i < n; ++i)
    RunSyncRaw(ex, [&] { ++raw; });
  EXPECT(raw == n);
}

// Several threads at once against one executor (the runtime thread and the
// engine's helper threads all call into the UI thread).
static void TestManyCallers() {
  Executor ex;
  constexpr int kThreads = 6;
  const int per_thread = Iterations(20000, 4000);
  long long total = 0;  // only touched on the executor thread
  std::vector<std::thread> callers;
  for (int t = 0; t < kThreads; ++t) {
    callers.emplace_back([&, t] {
      for (int i = 0; i < per_thread; ++i) {
        if ((i + t) % 2)
          RunSyncLambda(ex, [&] { ++total; });
        else
          RunSyncRaw(ex, [&] { ++total; });
      }
    });
  }
  for (auto& c : callers)
    c.join();
  long long seen = -1;
  RunSyncLambda(ex, [&] { seen = total; });
  EXPECT(seen == static_cast<long long>(kThreads) * per_thread);
}

// The result the task computes is visible to the waiter without any other
// synchronization.
static void TestResultVisible() {
  Executor ex;
  for (int i = 0; i < Iterations(20000, 2000); ++i) {
    std::vector<int> out;
    RunSyncRaw(ex, [&] { out.assign(static_cast<size_t>(i % 7) + 1, i); });
    EXPECT(out.size() == static_cast<size_t>(i % 7) + 1 && out.back() == i);
  }
}

// A task posted from the executor's own thread runs inline (no deadlock).
static void TestOnThreadRunsInline() {
  Executor ex;
  bool inner = false;
  RunSyncLambda(ex, [&] { RunSyncLambda(ex, [&] { inner = true; }); });
  EXPECT(inner);
}

int main() {
  Watchdog watchdog(std::chrono::seconds(120));
  TestBackToBack();
  TestManyCallers();
  TestResultVisible();
  TestOnThreadRunsInline();
  std::printf("sync_call_test: all tests passed\n");
  return 0;
}
