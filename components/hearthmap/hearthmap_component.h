// hearthmap_component.h — the ESPHome glue: Home Assistant states and the clock in, core in the
// middle, ui_lvgl's Screen out. Thin by design: every decision is core's (AGENTS.md principle 2),
// every pixel is ui_lvgl's, and the heavy work is PanelPipeline's, done in bounded slices from
// loop() so setup() returns at once (docs/decisions.md D-038). host/src/app.cpp is the same
// composition on the host.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "generated_photos.h"
#include "hearthmap/readings.h"
#include "hearthmap/ui/ports.h"
#include "hearthmap/ui/screen.h"
#include "hearthmap/view_model.h"
#include "hearthmap_gen/generated_config.h"
#include "panel_pipeline.h"
#include "perf_stats.h"
#ifdef USE_TOUCHSCREEN
#include "esphome/components/touchscreen/touchscreen.h"
#endif

namespace esphome::hearthmap_panel {

namespace hm = ::hearthmap;

/// Take over LVGL's log output (to ESPHome's log, as before) and count warnings and errors.
void hook_lvgl_log();
uint32_t lvgl_warnings();
uint32_t lvgl_errors();

#ifdef USE_TOUCHSCREEN
/// Tells PerfStats when the touchscreen reports a new contact, so the log can say how long LVGL
/// took to see it. Registered only when the YAML names the touchscreen (touchscreen_id).
class TouchProbe : public touchscreen::TouchListener {
 public:
  explicit TouchProbe(hearthmap_device::PerfStats* stats) : stats_(stats) {}
  void touch(touchscreen::TouchPoint tp) override;

 private:
  hearthmap_device::PerfStats* stats_;
};
#endif

class HearthmapComponent : public Component, public hm::ui::EventSink {
 public:
  void set_lvgl(lvgl::LvglComponent* lvgl) { lvgl_ = lvgl; }
#ifdef USE_TOUCHSCREEN
  void set_touchscreen(touchscreen::Touchscreen* ts) { touchscreen_ = ts; }
#endif
  void set_time(time::RealTimeClock* clock) { clock_ = clock; }
  void set_refresh_interval_ms(uint32_t ms) { refresh_interval_ms_ = ms; }
  void set_min_redraw_interval_ms(uint32_t ms) { min_redraw_interval_ms_ = ms; }
  void set_heat_refresh_interval_ms(uint32_t ms) { heat_refresh_interval_ms_ = ms; }

  // After LVGL (PROCESSOR) and the YAML-built widgets (PROCESSOR - 5).
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 10.0f; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// ui_lvgl calls this from inside an LVGL event callback: queue, act in loop() (ports.h).
  void on_ui_event(const hm::UiEvent& event) override { pending_.push_back(event); }

 private:
  enum class Phase : uint8_t { Booting, Failed, WaitingForTime, Running };

  void boot_slice_();
  bool start_screen_();
  void subscribe_();
  void frame_slice_();
  void refresh_view_(uint32_t start_ms, bool connected);
  void show_view_();
  void invalidate_band_();
  void handle_publish_();
  void show_status_(const char* text);
  void hide_status_();
  void hook_perf_();
  void log_perf_();
  static void on_display_event_(lv_event_t* e);
  static void on_indev_event_(lv_event_t* e);

  lvgl::LvglComponent* lvgl_ = nullptr;
  time::RealTimeClock* clock_ = nullptr;
  uint32_t refresh_interval_ms_ = 30000;
  uint32_t min_redraw_interval_ms_ = 500;
  uint32_t heat_refresh_interval_ms_ = 15000;

  Phase phase_ = Phase::Booting;
  hearthmap_gen::GeneratedConfigSource source_;  ///< build() feeds the pipeline
  std::unique_ptr<hearthmap_device::PanelPipeline> pipeline_;
  uint32_t boot_started_ms_ = 0;
  uint32_t stage_started_ms_ = 0;
  uint32_t stage_slices_ = 0;
  uint32_t stage_worst_us_ = 0;

  hm::Readings readings_;
  hm::UiState ui_state_;
  std::vector<hm::UiEvent> pending_;
  bool dirty_ = true;
  bool viewed_ = false;     ///< At least one view model has been shown.
  bool connected_ = false;  ///< The API connection state the last view was built with.
  bool first_heat_logged_ = false;
  bool heat_active_ = false;  ///< A heat rebuild is in flight (for the timing log).
  bool dimmed_ = false;       ///< The last Screen update hid the heat image as behind.
  uint32_t heat_started_ms_ = 0;
  uint32_t last_view_ms_ = 0;
  uint32_t last_sprite_frames_ = 0;
  uint32_t last_sprite_dropped_ = 0;
  bool rebake_requested_ = false;
  bool pressed_ = false;  ///< A press was seen and heat work is held (kHeatHoldAfterPressMs).
  uint32_t last_press_ms_ = 0;

  hm::Rgb565* heat_pixels_ = nullptr;  ///< rasterizer().pixel_count() pixels, in PSRAM.
  hm::Rgb565* heat_back_ = nullptr;    ///< The second buffer frames render into.
  int32_t band_row_ = 1 << 30;         ///< Next heat-image row to invalidate; past the end: none.
  uint32_t last_band_ms_ = 0;
  lv_image_dsc_t heat_dsc_{};
  hearthmap_device::GeneratedPhotos photos_;
  std::unique_ptr<hm::ui::Screen> screen_;
  lv_obj_t* status_ = nullptr;

  hearthmap_device::PerfStats perf_;  ///< Where the time goes; one DEBUG line per window.
#ifdef USE_TOUCHSCREEN
  touchscreen::Touchscreen* touchscreen_ = nullptr;
  std::unique_ptr<TouchProbe> touch_probe_;
#endif
};

}  // namespace esphome::hearthmap_panel
