// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The rendezvous behind the "run this on the UI thread and wait" helpers
// (cef_invoke_sync in cef/src/runtime_loader.cc, gtk_invoke_sync in
// webview/src/webview_linux.cc). Header-only so both backends share one
// implementation, and so backend-common/tests/sync_call_test.cc (run under
// ThreadSanitizer in CI) exercises the code they ship.
//
// Usage: the waiting thread owns a SyncCall in its own stack frame, hands a
// pointer to it with the task it posts, and calls Wait(); the task calls
// Done() as its very last access to the SyncCall:
//
//   laufey_common::SyncCall call;
//   PostToOtherThread([&] { fn(); call.Done(); });
//   call.Wait();
//
// Done() notifies while still holding the lock. Wait() can only return after
// it has re-acquired the mutex and seen `done_`, so once Done() unlocks, the
// waiter may return at once and destroy the SyncCall. A notify issued after
// the unlock could therefore reach a condition variable that is already gone,
// or the next call's SyncCall at the same stack address. On macOS that left
// the next wait asleep forever or crashed.

#ifndef LAUFEY_SYNC_CALL_H_
#define LAUFEY_SYNC_CALL_H_

#include <condition_variable>
#include <mutex>

namespace laufey_common {

class SyncCall {
 public:
  SyncCall() = default;
  SyncCall(const SyncCall&) = delete;
  SyncCall& operator=(const SyncCall&) = delete;

  // Marks the call complete and wakes the waiter. Must be the last thing the
  // task does with this object: the waiter may destroy it as soon as the lock
  // is released.
  void Done() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_one();
  }

  // Blocks until Done() has been called.
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return done_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
};

}  // namespace laufey_common

#endif  // LAUFEY_SYNC_CALL_H_
