// perf_stats.h — where the panel's time goes, counted cheaply and logged every few seconds.
//
// Pure C++17: no ESPHome, no LVGL, no clock. The component feeds it timestamps from LVGL's
// display and input-device events and from its own loop(), and logs one line per window at
// DEBUG (components/hearthmap/README.md, "Measuring"). Everything here is a handful of integer
// operations per event, so it stays on in normal builds: the numbers exist the day someone needs
// them, without a special firmware.
//
// Timestamps are the caller's monotonic clocks (micros() and millis() on the device); both may
// wrap, and every difference is taken in unsigned arithmetic so a wrap costs nothing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hearthmap_device {

/// One window's totals. Times in microseconds unless the name says ms.
struct PerfWindow {
  std::uint32_t window_ms = 0;  ///< Length of the window this covers.

  // The main loop, as seen from the component's loop(): one call per pass of ESPHome's loop.
  std::uint32_t loops = 0;         ///< loop() calls.
  std::uint32_t loop_gap_max = 0;  ///< Longest time between two loop() starts.
  std::uint32_t own_view = 0;      ///< In the component: view model and screen updates.
  std::uint32_t own_heat = 0;      ///< In the component: heat-rebuild steps.
  std::uint32_t own_max = 0;       ///< Longest single loop() of the component.

  // LVGL refreshes (render plus flush), from the display's REFR_START to REFR_READY. Only
  // refreshes that flushed something count.
  std::uint32_t refreshes = 0;
  std::uint32_t refresh_total = 0;
  std::uint32_t refresh_max = 0;
  std::uint32_t flushes = 0;         ///< flush_cb calls (one per rendered area or chunk).
  std::uint32_t flush_total = 0;     ///< Inside flush_cb: rotation and the copy to the panel.
  std::uint64_t flushed_px = 0;      ///< Pixels handed to flush_cb.
  std::uint64_t invalidated_px = 0;  ///< Pixels asked for by invalidations (before joining).

  // Touch. A "read" is the touchscreen reporting a new contact (only when the component is
  // given the touchscreen); "pressed"/"clicked" are LVGL's own input events.
  std::uint32_t touch_reads = 0;
  std::uint32_t presses = 0;
  std::uint32_t clicks = 0;
  std::uint32_t read_to_press_max_ms = 0;  ///< Touch read until LVGL saw the press.
  std::uint32_t acks = 0;                  ///< Clicks whose screen update has been drawn.
  std::uint32_t click_to_ack_max_ms = 0;   ///< LVGL click until the refresh showing its effect.

  // The longest refresh by what caused it: a tap's screen update, a new heat image being
  // repainted, or neither (animation: pulse sprites).
  std::uint32_t refresh_max_tap = 0;
  std::uint32_t refresh_max_heat = 0;
  std::uint32_t refresh_max_anim = 0;

  // Pulse sprites (ui_lvgl's Screen::pulse_counters()), set by the caller at the window's end.
  std::uint32_t sprite_frames = 0;    ///< frames shown in the window
  std::uint32_t sprite_dropped = 0;   ///< frames skipped in the window
  std::uint32_t sprite_kib = 0;       ///< memory the sprites hold now
  std::uint32_t sprite_frame_us = 0;  ///< choosing sprite frames (inside LVGL's timer handler)
  /// Capturing sprite backgrounds and composing their frames (prepare steps, inside hm heat).
  std::uint32_t sprite_prepare_us = 0;
  std::uint32_t sprite_divisor = 1;     ///< frames per cycle divided by this to fit memory
  std::uint32_t sprite_not_ready = 0;   ///< frames held back in the window (set incomplete)
  std::uint32_t sprite_canary = 0;      ///< frame sets found written out of bounds, since boot
  std::uint32_t presented = 0;          ///< pulse frames put on the panel directly (D-058)
  std::uint32_t present_fallbacks = 0;  ///< pulse frames shown by invalidating instead
  const char* present_block = "none";   ///< the most common reason a present was refused
};

class PerfStats {
 public:
  /// A click whose effect has not been drawn after this long is dropped from the latency
  /// figures (nothing on screen changed, or the panel never refreshed): it is not a latency.
  static constexpr std::uint32_t kAckTimeoutMs = 3000;

  explicit PerfStats(std::uint32_t window_ms = 5000) : window_ms_(window_ms) {}

  /// The component's loop() started (`now_us`) and, separately, how long its two kinds of work
  /// took in that call.
  void loop_start(std::uint32_t now_us);
  void own_work(std::uint32_t view_us, std::uint32_t heat_us);

  /// LVGL display events.
  void invalidated(std::uint32_t px) { w_.invalidated_px += px; }
  void refresh_start(std::uint32_t now_us);
  void flush_start(std::uint32_t now_us);
  void flush_finish(std::uint32_t now_us, std::uint32_t px);
  void refresh_ready(std::uint32_t now_us, std::uint32_t now_ms);

  /// Touch and taps. touch_read() is one new contact: it starts that contact's timing, replacing
  /// any earlier contact LVGL never saw pressed.
  void touch_read(std::uint32_t now_ms);
  void lvgl_pressed(std::uint32_t now_ms);
  void lvgl_clicked(std::uint32_t now_ms);
  /// What the next refresh draws: a tap's update, a heat repaint (otherwise: animation).
  void cause_tap() { cause_tap_ = true; }
  void cause_heat() { cause_heat_ = true; }

  /// The component applied the queued UI events and updated the screen; `changed` says whether
  /// that update changed anything on screen (a tap on empty map or on the shown tab does not,
  /// and is then not timed: the next refresh would be some pulse, not its answer).
  void events_applied(std::uint32_t now_ms, bool changed);

  /// True once `window_ms` has passed since the window started (the first call starts it).
  bool due(std::uint32_t now_ms);
  /// The finished window; starts the next one at `now_ms`.
  PerfWindow take(std::uint32_t now_ms);

  /// One log line for `w`, into `out` (always NUL-terminated); returns the length written.
  static std::size_t format(const PerfWindow& w, char* out, std::size_t size);

 private:
  std::uint32_t window_ms_;
  PerfWindow w_;
  bool started_ = false;
  std::uint32_t window_start_ms_ = 0;

  bool have_loop_ = false;
  std::uint32_t last_loop_us_ = 0;

  bool in_refresh_ = false;
  std::uint32_t refresh_start_us_ = 0;
  std::uint32_t refresh_flushes_ = 0;
  std::uint32_t flush_start_us_ = 0;

  bool cause_tap_ = false;
  bool cause_heat_ = false;
  bool read_pending_ = false;
  std::uint32_t read_ms_ = 0;
  bool click_pending_ = false;  ///< A click waits for the component to apply it.
  bool ack_pending_ = false;    ///< Applied; waits for the next refresh to draw it.
  std::uint32_t click_ms_ = 0;
};

}  // namespace hearthmap_device
