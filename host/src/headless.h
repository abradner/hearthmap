// headless.h — LVGL with no window: a display that renders into memory, a scripted pointer, and
// a clock the caller sets. Used by the CLI renderer and by ui_lvgl's widget tests.
//
// LVGL keeps global state, so one HeadlessDisplay may exist at a time per process.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lvgl.h"

namespace hearthmap::host {

class HeadlessDisplay {
 public:
  /// Initialises LVGL on first use, creates a `width` x `height` RGB565 display rendering
  /// directly into one full-screen buffer, and a pointer input device. The clock starts at 0 ms.
  HeadlessDisplay(std::int32_t width, std::int32_t height);
  ~HeadlessDisplay();
  HeadlessDisplay(const HeadlessDisplay&) = delete;
  HeadlessDisplay& operator=(const HeadlessDisplay&) = delete;

  lv_display_t* display() const { return display_; }
  lv_obj_t* screen() const;

  /// LVGL's tick, in milliseconds. Animations advance only when time moves and timers run.
  void set_time_ms(std::uint32_t ms);
  std::uint32_t time_ms() const;
  /// Run LVGL's due timers (animations, refresh) once at the current time.
  void run_timers();

  /// A press and release at (x, y) at the current time, delivered through LVGL's input
  /// pipeline exactly as a touch panel would deliver it.
  void tap(std::int32_t x, std::int32_t y);
  /// The two halves of tap(), for tests of what a press shows before it is released.
  void press(std::int32_t x, std::int32_t y);
  void release();

  /// Render the whole screen now into framebuffer().
  void render();

  std::int32_t width() const { return width_; }
  std::int32_t height() const { return height_; }
  /// RGB565, row-major, stride width().
  const std::vector<std::uint16_t>& framebuffer() const { return framebuffer_; }
  /// Size of LVGL's draw buffer here: one full frame (a device can use partial buffers).
  std::size_t draw_buffer_bytes() const { return framebuffer_.size() * sizeof(std::uint16_t); }

 private:
  std::int32_t width_;
  std::int32_t height_;
  std::vector<std::uint16_t> framebuffer_;
  lv_display_t* display_ = nullptr;
  lv_indev_t* pointer_ = nullptr;
};

}  // namespace hearthmap::host
