// worker.h — the port for work that may run off the main loop (docs/decisions.md D-069).
//
// Some of the panel's work is pure computation over buffers: composing pulse sprite frames from
// their captures, and building heat images. On a dual-core target it can run on the core the
// main loop is not using; on a single-core target, and on the host, it runs inline in small
// slices from the main loop, exactly as before. Callers cannot tell the two apart except by time.
//
// Nothing on a worker may touch LVGL (it is not thread-safe) or allocate through the memory port
// (hearthmap/ui/memory.h allocates from one task): a job computes into buffers its caller sized
// and allocated beforehand, and the caller adopts them once the job is back.
//
// The hand-off, per lane (a lane holds at most one job; lanes run independently):
//
//   Idle --submit()--> Queued --(the worker runs step() until it returns false, or until
//                                cancel() is seen between two steps)--> Done --poll()--> Idle
//
//   - While a lane is Queued the job object and everything it points to that the caller lent it
//     belong to the worker: the main loop must not read or write them (sizes it set before
//     submit() and the worker never changes may be read). That is the only rule; the worker
//     never touches anything else of the caller's.
//   - submit() publishes the job with a release store; the worker reads the lane with an acquire
//     load before its first step. The worker publishes Done with a release store after its last
//     step; poll() reads it with an acquire load. Everything the job wrote is therefore visible
//     to the main loop once poll() has said Finished, and everything the caller wrote before
//     submit() is visible to the job.
//   - cancel() only asks: the job keeps its buffers until the worker returns it (at most one step
//     later), and the caller must still poll() it back before reusing or freeing anything lent.
//     A caller whose inputs changed meanwhile tells the result apart by its own generation
//     counter (the job carries the generation it was built for); a stale result is discarded,
//     never adopted.
//   - Every call except the worker's own run_pending() is made from the main loop (one task).
//
// No lock is held while a step runs, and nothing here disables interrupts: the lane states are
// atomics, and the platform's wake-up (a task notification on the panel, a condition variable on
// the host) is its only blocking primitive, used by the worker thread alone.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hearthmap::ui {

/// Work done in bounded steps.
class WorkJob {
 public:
  virtual ~WorkJob() = default;
  /// One bounded unit (about a millisecond or less on the panel). Returns true while work remains.
  /// Runs on the worker, or inline on the main loop: touch only what the job owns or was lent.
  virtual bool step() = 0;
};

/// Which lane a client uses. Each lane holds one job at a time.
enum class WorkLane : std::uint8_t { Sprites = 0, Heat = 1 };

class Worker {
 public:
  static constexpr std::size_t kLanes = 2;

  /// What poll() found.
  enum class Poll : std::uint8_t {
    Idle,      ///< nothing was submitted on the lane
    Busy,      ///< the worker still holds the job
    Finished,  ///< the job is back with its caller (the lane is Idle again)
  };

  virtual ~Worker() = default;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  /// True when jobs run on another thread; false when they run inline from poll().
  bool threaded() const { return threaded_; }

  /// Hands `job` to the worker. False (nothing changes) when the lane already holds a job.
  bool submit(WorkLane lane, WorkJob& job);
  /// Asks the worker to stop the lane's job between two steps. Does nothing on an idle lane.
  void cancel(WorkLane lane);
  /// Whether the lane's job is back, and if so the lane is Idle again. Inline, it first runs one
  /// step of the job (or its cancellation) on the calling thread. `completed` (optional) is set
  /// to true when the job ran to its end, false when it was cancelled first.
  Poll poll(WorkLane lane, bool* completed = nullptr);
  /// The lane holds a job (submitted and not yet polled back).
  bool busy(WorkLane lane) const;

  /// Totals since construction, for the perf log. `busy_us` is time inside step() on the worker
  /// thread (zero when inline, where the caller times its own slices); wraps.
  struct Stats {
    std::uint32_t steps = 0;
    std::uint32_t completed = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t busy_us = 0;
  };
  Stats stats() const;

 protected:
  /// `micros` (a microsecond clock; may be nullptr) lets the worker thread rest every
  /// `rest_after_us` of continuous work, so lower-priority tasks on its core (the idle task the
  /// task watchdog watches) get to run.
  Worker(bool threaded, std::uint32_t (*micros)(), std::uint32_t rest_after_us);

  /// The worker thread's body calls this whenever it wakes: it runs every queued lane, a step
  /// at a time in turn, until none is queued. Worker thread only.
  void run_pending();
  /// Something was queued: wake the worker thread. Main loop only.
  virtual void wake() {}
  /// Give the core away briefly (run_pending() calls it after `rest_after_us` of work).
  virtual void rest() {}
  /// Any lane queued (for the platform's wait predicate).
  bool any_queued() const;

 private:
  enum : std::uint8_t { kIdle = 0, kQueued = 1, kDone = 2 };
  struct Lane {
    std::atomic<std::uint8_t> state{kIdle};
    std::atomic<bool> cancel{false};
    /// Written by the worker before its Done store, read by poll() after its acquire load.
    bool completed = false;
    /// Written by the main loop while Idle, before the Queued store.
    WorkJob* job = nullptr;
  };
  /// One step of the lane's job or its cancellation; true if the lane is now Done. Whoever holds
  /// the lane (the worker thread, or poll() when inline) calls it.
  bool run_one(Lane& l);

  const bool threaded_;
  std::uint32_t (*micros_)();
  std::uint32_t rest_after_us_;
  std::array<Lane, kLanes> lanes_{};
  std::atomic<std::uint32_t> steps_{0};
  std::atomic<std::uint32_t> completed_{0};
  std::atomic<std::uint32_t> cancelled_{0};
  std::atomic<std::uint32_t> busy_us_{0};
};

/// Runs every job inline from poll(), one step per call: the host's worker, a single-core
/// target's, and the panel's with `worker: false`. A client given no worker makes its own.
class InlineWorker final : public Worker {
 public:
  InlineWorker() : Worker(false, nullptr, 0) {}
};

}  // namespace hearthmap::ui
