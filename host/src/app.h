// app.h — the host's composition of core and ui_lvgl: what the ESPHome component will do on the
// device, minus Home Assistant. Holds the one piece of UI state (core's UiState), turns queued
// UI events into the next state with core's apply(), and redraws through the Screen.
//
// Refresh path (docs/decisions.md D-022): view model, samples, field, cell colours and the heat
// image are all written into storage this object owns, so a warm refresh allocates nothing in
// core or in the rasterizer.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/readings.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/ui/memory.h"
#include "hearthmap/ui/ports.h"
#include "hearthmap/ui/screen.h"
#include "hearthmap/view_model.h"
#include "lvgl.h"

namespace hearthmap::host {

/// The pulse sprites' budget on the panel, as the host models it: an estimate of what the panel
/// gets (its "boot: N KiB for pulse sprites" line: three quarters of the PSRAM left past the
/// reserve and the map bake), on the low side. The renderer and the perf proxy use it, so goldens
/// and budgets show the panel rather than a small target.
inline constexpr std::size_t kPanelPulseMemory = 8U * 1024U * 1024U;

class App final : public ui::EventSink {
 public:
  /// Builds the Screen under `parent`. `config` and `readings` are copied. `pulse_memory_bytes`
  /// is the pulse sprites' budget (0: the Screen's default, a small target's). `worker`
  /// (optional) computes sprite frames off the calling thread (D-069); nullptr: inline.
  App(lv_obj_t* parent, Config config, Readings readings, Timestamp now, int utc_offset_minutes,
      const ui::ImageProvider* photos, std::size_t pulse_memory_bytes = 0,
      ui::Worker* worker = nullptr);

  /// ui::EventSink: queue only (LVGL is mid-callback).
  void on_ui_event(const UiEvent& event) override { pending_.push_back(event); }

  /// Apply queued events in order, then refresh. Returns how many were applied.
  std::size_t process_events();

  /// Rebuild the view model (and the heat image when the layer or data changed) and update.
  void refresh();

  void set_layer(Layer layer);
  /// New readings at `now`: the view model and the heat image are rebuilt, as the panel does
  /// when sensors change.
  void set_readings(Readings readings, Timestamp now);
  const UiState& ui_state() const { return state_; }
  const ViewModel& view_model() const { return vm_; }
  const ui::Screen& screen() const { return *screen_; }
  ui::Screen& screen() { return *screen_; }
  std::size_t heat_image_bytes() const { return heat_pixels_.size() * sizeof(Rgb565); }
  std::size_t rasterizer_bytes() const { return raster_.working_bytes(); }

 private:
  void render_heat(Layer layer);

  Config config_;
  Readings readings_;
  Timestamp now_;
  int utc_offset_minutes_;
  UiState state_;
  RoomMask mask_;
  ui::HeatRasterizer raster_;
  std::array<ColourLut, kLayerCount> luts_;
  std::vector<HeatSample> samples_;
  HeatField field_;
  // The embedder's bulk storage goes through the memory port too (D-064), as on the panel.
  ui::BulkVector<Rgb565> cell_colours_;
  ui::BulkVector<Rgb565> heat_pixels_;
  lv_image_dsc_t heat_dsc_{};
  ViewModel vm_;
  std::optional<Layer> heat_layer_;
  bool heat_baked_ = false;
  std::vector<UiEvent> pending_;
  std::unique_ptr<ui::Screen> screen_;
};

}  // namespace hearthmap::host
