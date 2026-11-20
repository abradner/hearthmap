// hearthmap_component.h — the ESPHome glue: Home Assistant states and the clock in, core in the
// middle, ui_lvgl's Screen out. Thin by design: every decision is core's (AGENTS.md principle 2),
// every pixel is ui_lvgl's. host/src/app.cpp is the same composition on the host.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/readings.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/ui/ports.h"
#include "hearthmap/ui/screen.h"
#include "hearthmap/view_model.h"

namespace esphome::hearthmap_panel {

namespace hm = ::hearthmap;

/// Plant photos from the generated tables, as LVGL descriptors built once at boot.
class GeneratedPhotos final : public hm::ui::ImageProvider {
 public:
  void init();
  /// nullptr for an id with no photo: ui_lvgl then draws its placeholder leaf.
  const lv_image_dsc_t* plant_photo(std::string_view photo_id) const override;

 private:
  std::vector<lv_image_dsc_t> photos_;
};

class HearthmapComponent : public Component, public hm::ui::EventSink {
 public:
  void set_lvgl(lvgl::LvglComponent* lvgl) { lvgl_ = lvgl; }
  void set_time(time::RealTimeClock* clock) { clock_ = clock; }
  void set_refresh_interval_ms(uint32_t ms) { refresh_interval_ms_ = ms; }
  void set_min_redraw_interval_ms(uint32_t ms) { min_redraw_interval_ms_ = ms; }

  // After LVGL (PROCESSOR) and the YAML-built widgets (PROCESSOR - 5).
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 10.0f; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// ui_lvgl calls this from inside an LVGL event callback: queue, act in loop() (ports.h).
  void on_ui_event(const hm::UiEvent& event) override { pending_.push_back(event); }

 private:
  enum class Phase : uint8_t { ConfigError, WaitingForTime, Running };

  bool start_ui_();
  void subscribe_();
  void rebuild_(hm::Timestamp now);
  void show_status_(const char* text);
  void hide_status_();

  lvgl::LvglComponent* lvgl_ = nullptr;
  time::RealTimeClock* clock_ = nullptr;
  uint32_t refresh_interval_ms_ = 30000;
  uint32_t min_redraw_interval_ms_ = 500;

  Phase phase_ = Phase::ConfigError;
  hm::Config config_;
  hm::Readings readings_;
  hm::UiState ui_state_;
  std::vector<hm::UiEvent> pending_;
  bool dirty_ = true;
  uint32_t last_build_ms_ = 0;

  // Reuse buffers: once warm, a rebuild allocates nothing in core (D-022).
  hm::ViewModel view_;
  hm::RoomMask mask_;
  hm::HeatField field_;
  hm::HeatField shown_field_;  ///< The field the heat image was last drawn from.
  bool heat_valid_ = false;
  hm::Layer shown_layer_ = hm::Layer::Temperature;
  std::vector<hm::HeatSample> samples_;
  std::vector<hm::Rgb565> cell_colours_;
  std::array<hm::ColourLut, hm::kLayerCount> luts_;

  std::unique_ptr<hm::ui::HeatRasterizer> raster_;
  hm::Rgb565* heat_pixels_ = nullptr;  ///< raster_->pixel_count() pixels, in PSRAM.
  lv_image_dsc_t heat_dsc_{};
  GeneratedPhotos photos_;
  std::unique_ptr<hm::ui::Screen> screen_;
  lv_obj_t* status_ = nullptr;
};

}  // namespace esphome::hearthmap_panel
