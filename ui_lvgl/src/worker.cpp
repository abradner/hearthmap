#include "hearthmap/ui/worker.h"

namespace hearthmap::ui {

namespace {

// Every counter has one writer: the worker thread when threaded (run_one() and run_pending() run
// only there), the main loop when inline (poll() runs run_one() itself only then). So a counter
// is advanced with a load and a store, never a read-modify-write: no atomic RMW instruction is
// needed, which keeps an inline worker that lives in PSRAM (inside a PulseLayer, D-064) free of
// any question about atomic RMW on external memory. Readers on the other thread see a recent
// value (relaxed: these are statistics).
void bump(std::atomic<std::uint32_t>& counter, std::uint32_t by = 1U) {
  counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

}  // namespace

Worker::Worker(bool threaded, std::uint32_t (*micros)(), std::uint32_t rest_after_us)
    : threaded_(threaded), micros_(micros), rest_after_us_(rest_after_us) {}

bool Worker::submit(WorkLane lane, WorkJob& job) {
  Lane& l = lanes_[static_cast<std::size_t>(lane)];
  if (l.state.load(std::memory_order_acquire) != kIdle) return false;
  l.job = &job;
  l.completed = false;
  l.cancel.store(false, std::memory_order_relaxed);
  // Publishes the job and everything its caller wrote before this call.
  l.state.store(kQueued, std::memory_order_release);
  wake();
  return true;
}

void Worker::cancel(WorkLane lane) {
  Lane& l = lanes_[static_cast<std::size_t>(lane)];
  if (l.state.load(std::memory_order_relaxed) == kIdle) return;
  l.cancel.store(true, std::memory_order_relaxed);
}

bool Worker::busy(WorkLane lane) const {
  return lanes_[static_cast<std::size_t>(lane)].state.load(std::memory_order_relaxed) != kIdle;
}

bool Worker::run_one(Lane& l) {
  if (l.cancel.load(std::memory_order_relaxed)) {
    l.completed = false;
    bump(cancelled_);
    l.state.store(kDone, std::memory_order_release);
    return true;
  }
  const bool more = l.job->step();
  bump(steps_);
  if (more) return false;
  l.completed = true;
  bump(completed_);
  // Publishes everything the job wrote.
  l.state.store(kDone, std::memory_order_release);
  return true;
}

Worker::Poll Worker::poll(WorkLane lane, bool* completed) {
  Lane& l = lanes_[static_cast<std::size_t>(lane)];
  std::uint8_t s = l.state.load(std::memory_order_acquire);
  if (s == kIdle) return Poll::Idle;
  if (s == kQueued && !threaded_) {
    (void)run_one(l);  // inline: this thread holds the lane
    s = l.state.load(std::memory_order_acquire);
  }
  if (s != kDone) return Poll::Busy;
  if (completed != nullptr) *completed = l.completed;
  l.job = nullptr;
  l.state.store(kIdle, std::memory_order_relaxed);
  return Poll::Finished;
}

bool Worker::any_queued() const {
  for (const Lane& l : lanes_) {
    if (l.state.load(std::memory_order_acquire) == kQueued) return true;
  }
  return false;
}

void Worker::run_pending() {
  std::uint32_t since_rest = micros_ != nullptr ? micros_() : 0U;
  for (;;) {
    bool any = false;
    for (Lane& l : lanes_) {
      if (l.state.load(std::memory_order_acquire) != kQueued) continue;
      any = true;
      const std::uint32_t t0 = micros_ != nullptr ? micros_() : 0U;
      (void)run_one(l);
      if (micros_ != nullptr) {
        const std::uint32_t t1 = micros_();
        bump(busy_us_, t1 - t0);
        if (rest_after_us_ != 0U && t1 - since_rest >= rest_after_us_) {
          rest();
          since_rest = micros_();
        }
      }
    }
    if (!any) return;
  }
}

Worker::Stats Worker::stats() const {
  Stats s;
  s.steps = steps_.load(std::memory_order_relaxed);
  s.completed = completed_.load(std::memory_order_relaxed);
  s.cancelled = cancelled_.load(std::memory_order_relaxed);
  s.busy_us = busy_us_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace hearthmap::ui
