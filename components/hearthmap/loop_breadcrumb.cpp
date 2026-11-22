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
    case Crumb::HmFrames: return "hearthmap: showing pulse frames";
    case Crumb::HmView: return "hearthmap: view model and screen update";
    case Crumb::HmPrepare: return "hearthmap: map bake band or pulse sprite capture/compose";
    case Crumb::HmHeat: return "hearthmap: heat image step";
    case Crumb::HmBoot: return "hearthmap: boot step";
  }
  return "unknown";
}

LoopBreadcrumb& breadcrumb() {
  static LoopBreadcrumb b;
  return b;
}

Crumb write_crumb(Crumb c, std::uint32_t since_ms) {
  LoopBreadcrumb& b = breadcrumb();
  const std::uint32_t s = b.seq.load(std::memory_order_relaxed);
  b.seq.store(s + 1U, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const auto prev =
      static_cast<Crumb>(b.crumb.exchange(static_cast<std::uint8_t>(c), std::memory_order_relaxed));
  b.since_ms.store(since_ms, std::memory_order_relaxed);
  b.seq.store(s + 2U, std::memory_order_release);
  return prev;
}

CrumbSample read_crumb() {
  // A few tries: the writer holds the lock for two stores, and only the loop task writes. Past
  // them the last read is returned, marked as not a pair.
  LoopBreadcrumb& b = breadcrumb();
  CrumbSample out;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const std::uint32_t s1 = b.seq.load(std::memory_order_acquire);
    out.crumb = static_cast<Crumb>(b.crumb.load(std::memory_order_relaxed));
    out.since_ms = b.since_ms.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t s2 = b.seq.load(std::memory_order_relaxed);
    if (s1 == s2 && (s1 & 1U) == 0U) {
      out.paired = true;
      break;
    }
  }
  return out;
}

// Only the loop task writes, so its own plain reads of the pair are consistent.
CrumbScope::CrumbScope(Crumb c, std::uint32_t now_ms)
    : prev_(breadcrumb().crumb.load(std::memory_order_relaxed)),
      prev_since_(breadcrumb().since_ms.load(std::memory_order_relaxed)) {
  (void)write_crumb(c, now_ms);
}

CrumbScope::~CrumbScope() {
  (void)write_crumb(static_cast<Crumb>(prev_), prev_since_);
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
  // A heartbeat ahead of `now_ms` (sampled in the wrong order) is no stall, not a 49-day one.
  const std::uint32_t stalled = elapsed_ms(now_ms, turn_ms);
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
