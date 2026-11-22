// loop_breadcrumb.h — which blocking call ESPHome's main loop task is in, for the stall monitor
// (stall_monitor.h, docs/decisions.md D-062). One relaxed atomic store per mark: nothing in the
// fast path waits or allocates. Pure C++ (no ESPHome, no FreeRTOS), so the judging is host-tested.
#pragma once

#include <atomic>
#include <cstdint>

namespace esphome::hearthmap_panel {

/// Where the loop task is. Elsewhere means outside hearthmap and LVGL's refresh: another
/// component (Wi-Fi and its co-processor RPCs, the API, touch, the logger, ...).
enum class Crumb : std::uint8_t {
  Elsewhere,
  Hearthmap,    ///< hearthmap's own loop()
  LvglRefresh,  ///< LVGL rendering (between LV_EVENT_REFR_START and REFR_READY)
  LvglFlush,    ///< ESPHome's flush: PPA rotate, draw_pixels_at, the DMA2D completion wait
  Present,      ///< DisplayPresenter: draw_pixels_at for a tile
  PpaFill,      ///< PpaBlitter: a blocking PPA fill
  PpaBlend,     ///< PpaBlitter: a blocking PPA blend
  PpaCopy,      ///< PpaBlitter: a blocking PPA copy (blend engine)
  // Inside hearthmap's loop(), its own steps, so a stall there is named to the step:
  HmFrames,   ///< showing due pulse frames (Screen::show_due_frames; presents nest as Present)
  HmView,     ///< the view model and the screen update
  HmPrepare,  ///< one map-bake band or one sprite capture or frame (Screen::prepare_step)
  HmHeat,     ///< one heat-image step (PanelPipeline::heat_step)
  HmBoot,     ///< one boot step (PanelPipeline::boot_step)
};

const char* crumb_name(Crumb c);

struct LoopBreadcrumb {
  std::atomic<std::uint8_t> crumb{0};
  std::atomic<std::uint32_t> turn_ms{0};   ///< millis() when hearthmap's loop() last started
  std::atomic<std::uint32_t> since_ms{0};  ///< millis() when the current crumb was set
  /// Odd while the loop task is writing crumb and since_ms (one writer: a sequence lock), so
  /// read_crumb() can take the two as a pair.
  std::atomic<std::uint32_t> seq{0};
};

/// The loop task's write of a crumb and its time, as one pair (read_crumb()); returns the crumb
/// it replaced.
Crumb write_crumb(Crumb c, std::uint32_t since_ms);

LoopBreadcrumb& breadcrumb();

/// `now_ms - then_ms` on one wrapping millisecond clock; 0 when `then_ms` is ahead of `now_ms`
/// (a timestamp another task stored after `now_ms` was read), never a 49-day age.
inline std::uint32_t elapsed_ms(std::uint32_t now_ms, std::uint32_t then_ms) {
  const std::uint32_t d = now_ms - then_ms;
  return d > 0x80000000u ? 0U : d;
}

/// A crumb and the time it was set, read from another task.
struct CrumbSample {
  Crumb crumb = Crumb::Elsewhere;
  std::uint32_t since_ms = 0;
  bool paired = false;  ///< false: the loop kept writing; crumb and time may not belong together
};
CrumbSample read_crumb();

/// Sets the crumb (for event pairs that are not one scope, such as LVGL's refresh and flush
/// events) and returns the one it replaced.
inline Crumb mark_crumb(Crumb c, std::uint32_t now_ms) {
  return write_crumb(c, now_ms);
}

/// Marks `c` for the scope's lifetime, then restores what was there. `now_ms` is the caller's
/// clock (millis() on the device).
class CrumbScope {
 public:
  CrumbScope(Crumb c, std::uint32_t now_ms);
  ~CrumbScope();
  CrumbScope(const CrumbScope&) = delete;
  CrumbScope& operator=(const CrumbScope&) = delete;

 private:
  std::uint8_t prev_;
  std::uint32_t prev_since_;
};

/// Decides what to log about one stall. A stall is time since the loop last turned. It warns
/// once past `warn_ms`, reports an error once past `error_ms` (set just under the task watchdog),
/// and says once that the loop turned again after a warning.
class StallJudge {
 public:
  enum class Verdict : std::uint8_t { None, Warn, Error, Recovered };
  StallJudge(std::uint32_t warn_ms, std::uint32_t error_ms)
      : warn_ms_(warn_ms), error_ms_(error_ms) {}
  /// `now_ms`, `turn_ms` from one clock; wrap-safe. `*stalled_ms` receives the stall length.
  Verdict step(std::uint32_t now_ms, std::uint32_t turn_ms, std::uint32_t* stalled_ms);

 private:
  std::uint32_t warn_ms_;
  std::uint32_t error_ms_;
  std::uint32_t last_turn_ = 0;
  std::uint32_t longest_ = 0;
  bool warned_ = false;
  bool errored_ = false;
};

}  // namespace esphome::hearthmap_panel
