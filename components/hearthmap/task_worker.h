// task_worker.h — the panel's Worker (ui_lvgl's worker.h, docs/decisions.md D-069): a FreeRTOS
// task pinned to the core the main loop does not run on.
//
// Only the thread is here; the lanes and the hand-off are worker.h's portable code, tested on the
// host with a std::thread (worker_thread.h, under ThreadSanitizer in CI). The task waits on its
// notification, runs every queued lane a step at a time until none is left, and gives its core
// away for a tick after every 10 ms of continuous work, so the idle task the task watchdog
// watches on that core still runs. Nothing it does disables interrupts: a task notification is
// its only blocking call.
//
// Its stack (kStackBytes, internal RAM, the one internal allocation the worker makes; never PSRAM,
// since flash and OTA writes disable the cache) holds a step's locals: a frame's layer lists
// (`Overlay[32]` and `RingLayer[32]`, about 2 KiB), or a heat unit's (core's field rows, the
// rasterizer's rows, the bake's blend, and the rasterizer's `Prepared`, about 1 KiB, returned by
// value and so a temporary on this stack for that unit), plus the task's saved FPU and PIE
// context. 16 KiB is about four
// times the largest of those as read from the code; it has not been measured, so the perf line
// reports how much was never touched (`stack free`), to be checked on the panel. An overflow
// would panic on every boot. The component creates the task only if free internal RAM stays
// above the internal-RAM floor plus its margin afterwards (InternalRamGuard::affords()).
#pragma once

#include <cstdint>

#include "hearthmap/ui/worker.h"

namespace esphome::hearthmap_panel {

class TaskWorker final : public ::hearthmap::ui::Worker {
 public:
  static constexpr std::uint32_t kStackBytes = 16384;
  static constexpr std::uint32_t kRestAfterUs = 10000;

  TaskWorker();
  ~TaskWorker() override;  // never on the panel (the component lives for the firmware's life)
  TaskWorker(const TaskWorker&) = delete;
  TaskWorker& operator=(const TaskWorker&) = delete;

  /// Creates the task on `core`. False (nothing created) when FreeRTOS cannot, e.g. no internal
  /// RAM for its stack: the caller then runs jobs inline instead.
  bool start(int core);
  int core() const { return core_; }
  /// Bytes of the task's stack never used since it started (FreeRTOS's high-water mark; it reads
  /// the stack's fill pattern, no lock). 0 before start().
  std::uint32_t stack_free() const;

 private:
  static void body(void* self);
  void wake() override;
  void rest() override;

  void* task_ = nullptr;  // TaskHandle_t, kept opaque so this header needs no FreeRTOS
  int core_ = -1;
};

}  // namespace esphome::hearthmap_panel
