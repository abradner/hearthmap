// panel_pipeline.h — the device's work, split into bounded steps so nothing blocks for long.
//
// Pure C++17 over core and ui_lvgl's HeatRasterizer: no ESPHome, no LVGL objects, no clock. The
// ESPHome component drives it from loop(), doing steps until a time budget is spent and then
// yielding, so the task watchdog, Wi-Fi and LVGL keep running (docs/decisions.md D-038). The
// host test (tests/test_cpp.py) drives the same class on a config at the schema's maximums and
// times every step.
//
// Boot:   Load (ConfigSource::load(), which runs validate(); colour tables)
//         -> Mask (core's RoomScanner, `rows` grid rows per step)
//         -> Raster (HeatRasterizer::build_rows(), `rows` rows per step)
//         -> Ready, or Refused when validate() reported an Error.
// Frame:  begin_frame() builds the view model and heat samples (core, reuse forms), then
//         frame_step() computes `rows` heat-field rows per step and, once the field is complete,
//         redraws the heat image only if the field or the layer changed.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/readings.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/view_model.h"

namespace hearthmap_device {

class PanelPipeline {
 public:
  /// Heat-image pixel rows per frame_step() for each field row it would compute: a pixel row
  /// is far cheaper than a field row (integer blending versus a weight per sample).
  static constexpr std::int32_t kImageRowsPerFieldRow = 8;

  enum class Stage : std::uint8_t { Load, Mask, Raster, Ready, Refused };

  /// `source` and `palette` are used during Load only.
  PanelPipeline(const hearthmap::ConfigSource& source, const hearthmap::Palette& palette)
      : source_(&source), palette_(palette) {}

  Stage stage() const { return stage_; }
  static const char* stage_name(Stage stage);

  /// One bounded unit of boot work (see the file comment). `rows` >= 1. Returns the new stage.
  Stage boot_step(std::int32_t rows);

  /// Rows the current boot stage has done and has in total (for progress logs).
  std::int32_t stage_rows_done() const { return rows_done_; }
  std::int32_t stage_rows_total() const;

  /// Load's issues (warnings, and the errors when Refused).
  const std::vector<hearthmap::ConfigIssue>& issues() const { return issues_; }

  // Valid once Ready. The config lives here for the pipeline's lifetime.
  const hearthmap::Config& config() const { return config_; }
  const hearthmap::RoomMask& mask() const { return mask_; }
  const hearthmap::ui::HeatRasterizer& rasterizer() const { return *raster_; }

  /// The heat image: rasterizer().pixel_count() pixels the caller owns (PSRAM on the device).
  void set_heat_buffer(hearthmap::Rgb565* pixels) { heat_pixels_ = pixels; }

  /// Start a frame: view model and heat samples for `now`. Ready only.
  void begin_frame(const hearthmap::Readings& readings, hearthmap::Timestamp now,
                   const hearthmap::UiState& state);
  /// Continue the frame: up to `rows` heat-field rows per step, then (only if the field or the
  /// layer changed) the cell colours and then `rows` * kImageRowsPerFieldRow heat-image rows per
  /// step. Returns true when the frame is complete.
  bool frame_step(std::int32_t rows);
  /// After frame_step() returned true: whether this frame rewrote the heat image.
  bool heat_redrawn() const { return heat_redrawn_; }
  const hearthmap::ViewModel& view() const { return view_; }

 private:
  const hearthmap::ConfigSource* source_;
  hearthmap::Palette palette_;
  Stage stage_ = Stage::Load;
  std::int32_t rows_done_ = 0;
  std::vector<hearthmap::ConfigIssue> issues_;

  hearthmap::Config config_;
  hearthmap::RoomMask mask_;
  std::unique_ptr<hearthmap::RoomScanner> scanner_;
  std::unique_ptr<hearthmap::ui::HeatRasterizer> raster_;
  std::array<hearthmap::ColourLut, hearthmap::kLayerCount> luts_;

  hearthmap::Rgb565* heat_pixels_ = nullptr;
  hearthmap::ViewModel view_;
  std::vector<hearthmap::HeatSample> samples_;
  hearthmap::HeatField field_;
  hearthmap::HeatField shown_field_;  ///< The field the heat image was last drawn from.
  bool heat_valid_ = false;
  hearthmap::Layer shown_layer_ = hearthmap::Layer::Temperature;
  std::vector<hearthmap::Rgb565> cell_colours_;
  std::int32_t field_row_ = 0;
  std::int32_t image_row_ = -1;  ///< -1: not started; then pixel rows of the heat image done.
  bool field_ok_ = false;
  bool frame_done_ = true;
  bool heat_redrawn_ = false;
};

}  // namespace hearthmap_device
