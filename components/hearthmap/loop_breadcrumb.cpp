#include "loop_breadcrumb.h"

namespace esphome::hearthmap_panel {

const char* crumb_name(Crumb c) {
  switch (c) {
    case Crumb::Elsewhere:
      return "elsewhere in the main loop (another component: Wi-Fi/co-processor RPC, API, touch, "
             "...)";
    case Crumb::Hearthmap: return "hearthmap loop()";
    case Crumb::LvglRefresh: return "LVGL rendering";
    case Crumb::LvglFlush: return "LVGL flush (PPA rotate, draw_pixels_at, DMA2D completion wait)";
    case Crumb::Present: return "present: draw_pixels_at for a tile";
    case Crumb::PpaFill: return "Blitter: PPA fill";
    case Crumb::PpaBlend: return "Blitter: PPA blend";
    case Crumb::PpaCopy: return "Blitter: PPA copy";
  }
  return "unknown";
}

LoopBreadcrumb& breadcrumb() {
  static LoopBreadcrumb b;
  return b;
}

CrumbScope::CrumbScope(Crumb c, std::uint32_t now_ms)
    : prev_(breadcrumb().crumb.load(std::memory_order_relaxed)),
      prev_since_(breadcrumb().since_ms.load(std::memory_order_relaxed)) {
  breadcrumb().since_ms.store(now_ms, std::memory_order_relaxed);
  breadcrumb().crumb.store(static_cast<std::uint8_t>(c), std::memory_order_relaxed);
}

CrumbScope::~CrumbScope() {
  breadcrumb().crumb.store(prev_, std::memory_order_relaxed);
  breadcrumb().since_ms.store(prev_since_, std::memory_order_relaxed);
}

StallJudge::Verdict StallJudge::step(std::uint32_t now_ms, std::uint32_t turn_ms,
                                     std::uint32_t* stalled_ms) {
  if (turn_ms != last_turn_) {
    // The loop turned since the last look: close any stall in progress.
    const bool was_warned = warned_;
    const std::uint32_t longest = longest_;
    last_turn_ = turn_ms;
    warned_ = errored_ = false;
    longest_ = 0;
    if (was_warned) {
      if (stalled_ms != nullptr) *stalled_ms = longest;
      return Verdict::Recovered;
    }
  }
  // Unsigned, so wrap-safe; a difference past half the range means the heartbeat is ahead of
  // `now_ms` (sampled in the wrong order), which is no stall rather than a 49-day one.
  std::uint32_t stalled = now_ms - turn_ms;
  if (stalled > 0x80000000u) stalled = 0;
  if (stalled > longest_) longest_ = stalled;
  if (stalled_ms != nullptr) *stalled_ms = stalled;
  if (!errored_ && stalled >= error_ms_) {
    errored_ = warned_ = true;
    return Verdict::Error;
  }
  if (!warned_ && stalled >= warn_ms_) {
    warned_ = true;
    return Verdict::Warn;
  }
  return Verdict::None;
}

}  // namespace esphome::hearthmap_panel
