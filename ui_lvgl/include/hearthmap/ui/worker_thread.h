// worker_thread.h — a Worker on a std::thread (docs/decisions.md D-069), for the host's tests.
//
// Header-only on purpose: ui_lvgl's library never includes it, so no target is made to depend on
// a threads library it does not use. The panel's worker is a FreeRTOS task with the same body
// (components/hearthmap/task_worker.cpp); this one exists so the hand-off in worker.h runs
// against a real second thread on the host, under ThreadSanitizer where the build enables it.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "hearthmap/ui/worker.h"

namespace hearthmap::ui {

class ThreadWorker final : public Worker {
 public:
  ThreadWorker() : Worker(true, &steady_micros, 10000U), thread_([this] { body(); }) {}
  ~ThreadWorker() override {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }
  ThreadWorker(const ThreadWorker&) = delete;
  ThreadWorker& operator=(const ThreadWorker&) = delete;

 private:
  static std::uint32_t steady_micros() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
  }
  void wake() override {
    // The lock orders this notify after the worker's predicate check, so no wake-up is lost.
    {
      const std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_one();
  }
  void rest() override { std::this_thread::yield(); }
  void body() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || any_queued(); });
        if (stop_) return;
      }
      run_pending();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;  // last: started once everything it uses exists
};

}  // namespace hearthmap::ui
