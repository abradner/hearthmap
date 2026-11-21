#include "perf_stats.h"

#include <cinttypes>
#include <cstdio>

namespace hearthmap_device {

namespace {

std::uint32_t max_u32(std::uint32_t a, std::uint32_t b) {
  return a > b ? a : b;
}

}  // namespace

void PerfStats::loop_start(std::uint32_t now_us) {
  ++w_.loops;
  if (have_loop_) w_.loop_gap_max = max_u32(w_.loop_gap_max, now_us - last_loop_us_);
  have_loop_ = true;
  last_loop_us_ = now_us;
}

void PerfStats::own_work(std::uint32_t view_us, std::uint32_t heat_us) {
  w_.own_view += view_us;
  w_.own_heat += heat_us;
  w_.own_max = max_u32(w_.own_max, view_us + heat_us);
}

void PerfStats::refresh_start(std::uint32_t now_us) {
  in_refresh_ = true;
  refresh_start_us_ = now_us;
  refresh_flushes_ = 0;
}

void PerfStats::flush_start(std::uint32_t now_us) {
  flush_start_us_ = now_us;
}

void PerfStats::flush_finish(std::uint32_t now_us, std::uint32_t px) {
  ++w_.flushes;
  ++refresh_flushes_;
  w_.flush_total += now_us - flush_start_us_;
  w_.flushed_px += px;
}

void PerfStats::refresh_ready(std::uint32_t now_us, std::uint32_t now_ms) {
  if (!in_refresh_) return;
  in_refresh_ = false;
  if (refresh_flushes_ == 0) return;  // nothing was drawn: not a refresh worth counting
  const std::uint32_t took = now_us - refresh_start_us_;
  ++w_.refreshes;
  w_.refresh_total += took;
  w_.refresh_max = max_u32(w_.refresh_max, took);
  if (ack_pending_) {
    ack_pending_ = false;
    const std::uint32_t latency = now_ms - click_ms_;
    if (latency <= kAckTimeoutMs) {
      ++w_.acks;
      w_.click_to_ack_max_ms = max_u32(w_.click_to_ack_max_ms, latency);
    }
  }
}

void PerfStats::touch_read(std::uint32_t now_ms) {
  ++w_.touch_reads;
  read_pending_ = true;
  read_ms_ = now_ms;
}

void PerfStats::lvgl_pressed(std::uint32_t now_ms) {
  ++w_.presses;
  if (read_pending_) {
    read_pending_ = false;
    w_.read_to_press_max_ms = max_u32(w_.read_to_press_max_ms, now_ms - read_ms_);
  }
}

void PerfStats::lvgl_clicked(std::uint32_t now_ms) {
  ++w_.clicks;
  click_pending_ = true;
  ack_pending_ = false;
  click_ms_ = now_ms;
}

void PerfStats::events_applied(std::uint32_t now_ms, bool changed) {
  if (!click_pending_) return;
  click_pending_ = false;
  if (changed && now_ms - click_ms_ <= kAckTimeoutMs) ack_pending_ = true;
}

bool PerfStats::due(std::uint32_t now_ms) {
  if (!started_) {
    started_ = true;
    window_start_ms_ = now_ms;
    return false;
  }
  return now_ms - window_start_ms_ >= window_ms_;
}

PerfWindow PerfStats::take(std::uint32_t now_ms) {
  PerfWindow out = w_;
  out.window_ms = started_ ? now_ms - window_start_ms_ : 0;
  w_ = PerfWindow{};
  started_ = true;
  window_start_ms_ = now_ms;
  return out;
}

std::size_t PerfStats::format(const PerfWindow& w, char* out, std::size_t size) {
  if (out == nullptr || size == 0) return 0;
  const std::uint32_t secs_x10 = w.window_ms == 0 ? 1 : (w.window_ms + 50) / 100;
  // Per-second rates, from the window's length (one decimal of seconds is plenty).
  auto per_s = [secs_x10](std::uint64_t v) { return (v * 10U + secs_x10 / 2) / secs_x10; };
  const int n = std::snprintf(
      out, size,
      "perf %" PRIu32 " ms: loop %" PRIu64 "/s, gap max %" PRIu32 " ms | lvgl %" PRIu32
      " refr, %" PRIu32 " ms (%" PRIu64 "%%), max %" PRIu32 " ms, flush %" PRIu32 " ms in %" PRIu32
      ", %" PRIu64 " kpx/s flushed, %" PRIu64 " kpx/s invalidated | hm view %" PRIu32
      " ms, heat %" PRIu32 " ms, max %" PRIu32 " ms | touch %" PRIu32 " read, %" PRIu32
      " press, %" PRIu32 " click, read>press max %" PRIu32 " ms, click>drawn max %" PRIu32
      " ms (%" PRIu32 ")",
      w.window_ms, per_s(w.loops), w.loop_gap_max / 1000U, w.refreshes, w.refresh_total / 1000U,
      w.window_ms == 0 ? 0 : (static_cast<std::uint64_t>(w.refresh_total) / 10U) / w.window_ms,
      w.refresh_max / 1000U, w.flush_total / 1000U, w.flushes, per_s(w.flushed_px) / 1000U,
      per_s(w.invalidated_px) / 1000U, w.own_view / 1000U, w.own_heat / 1000U, w.own_max / 1000U,
      w.touch_reads, w.presses, w.clicks, w.read_to_press_max_ms, w.click_to_ack_max_ms, w.acks);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(n) < size ? static_cast<std::size_t>(n) : size - 1;
}

}  // namespace hearthmap_device
