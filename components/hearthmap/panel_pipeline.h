// panel_pipeline.h — the device's work, split into bounded steps so nothing blocks for long.
//
// Pure C++17 over core and ui_lvgl's HeatRasterizer: no ESPHome, no LVGL objects, no clock. The
// ESPHome component drives it from loop(), doing steps until a time budget is spent and then
// yielding, so the task watchdog, Wi-Fi and LVGL keep running (docs/decisions.md D-038). The
// host test (tests/test_cpp.py) drives the same class on configs at the schema's maximums and
// bounds every step.
//
// Boot, one unit per boot_step():
//   Build     the generated tables into a Config, one room or pin per step (no validation yet)
//   Validate  core's ConfigValidator: the header, then one room, then one pin per step
//   Prepare   one layer's colour table per step, then the room scanner
//   Mask      `rows` heat-grid rows per step (core's RoomScanner)
//   Raster    `rows` rows per step (HeatRasterizer::build_rows())
//   -> Ready, or Refused when validation reported an Error.
// Frame: begin_frame() builds the view model and heat samples (core, reuse forms); frame_step()
// computes `rows` heat-field rows per step, then, only if the field or the layer changed, the
// cell colours and `rows` x kImageRowsPerFieldRow heat-image rows per step into the BACK buffer.
// When the frame completes the buffers swap, so the image the screen shows is never half
// written (published() says it happened).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
  enum class Stage : std::uint8_t { Build, Validate, Prepare, Mask, Raster, Ready, Refused };

  /// Heat-image pixel rows per frame_step() for each field row it would compute: a pixel row
  /// is far cheaper than a field row (integer blending versus a weight per sample).
  static constexpr std::int32_t kImageRowsPerFieldRow = 8;

  /// How to build the unvalidated config a unit at a time (the generated tables' build_steps()
  /// and build_step()).
  struct Builder {
    std::size_t steps = 0;
    std::function<void(hearthmap::Config&, std::size_t)> step;
  };

  /// `palette` colours the heat field. Both are used during boot only.
  PanelPipeline(Builder build, const hearthmap::Palette& palette)
      : build_(std::move(build)), palette_(palette) {}

  Stage stage() const { return stage_; }
  static const char* stage_name(Stage stage);

  /// One bounded unit of boot work (see the file comment). `rows` >= 1. Returns the new stage.
  Stage boot_step(std::int32_t rows);

  /// Units the current boot stage has done and has in total (for progress logs).
  std::int32_t stage_done() const { return done_; }
  std::int32_t stage_total() const;

  /// Validation's issues (warnings, and the errors when Refused).
  const std::vector<hearthmap::ConfigIssue>& issues() const { return issues_; }

  // Valid once Ready. The config lives here for the pipeline's lifetime.
  const hearthmap::Config& config() const { return config_; }
  const hearthmap::RoomMask& mask() const { return mask_; }
  const hearthmap::ui::HeatRasterizer& rasterizer() const { return *raster_; }

  /// Two heat-image buffers of rasterizer().pixel_count() pixels each, owned by the caller
  /// (PSRAM on the device). front() is the one to show; frames render into the other.
  void set_heat_buffers(hearthmap::Rgb565* a, hearthmap::Rgb565* b) {
    front_ = a;
    back_ = b;
  }
  hearthmap::Rgb565* front() const { return front_; }

  /// Start a frame: view model and heat samples for `now`. Ready only.
  void begin_frame(const hearthmap::Readings& readings, hearthmap::Timestamp now,
                   const hearthmap::UiState& state);
  /// Continue the frame (see the file comment). Returns true when the frame is complete.
  bool frame_step(std::int32_t rows);
  /// After frame_step() returned true: whether this frame swapped in a new heat image.
  bool published() const { return published_; }
  const hearthmap::ViewModel& view() const { return view_; }

 private:
  Builder build_;
  hearthmap::Palette palette_;
  Stage stage_ = Stage::Build;
  std::int32_t done_ = 0;
  std::vector<hearthmap::ConfigIssue> issues_;

  hearthmap::Config config_;
  std::unique_ptr<hearthmap::ConfigValidator> validator_;
  hearthmap::RoomMask mask_;
  std::unique_ptr<hearthmap::RoomScanner> scanner_;
  std::unique_ptr<hearthmap::ui::HeatRasterizer> raster_;
  std::array<hearthmap::ColourLut, hearthmap::kLayerCount> luts_;

  hearthmap::Rgb565* front_ = nullptr;
  hearthmap::Rgb565* back_ = nullptr;
  hearthmap::ViewModel view_;
  std::vector<hearthmap::HeatSample> samples_;
  hearthmap::HeatField field_;
  hearthmap::HeatField shown_field_;  ///< The field the front image was drawn from.
  bool heat_valid_ = false;
  hearthmap::Layer shown_layer_ = hearthmap::Layer::Temperature;
  std::vector<hearthmap::Rgb565> cell_colours_;
  std::int32_t field_row_ = 0;
  std::int32_t image_row_ = -1;  ///< -1: not started; then back-buffer rows done.
  bool field_ok_ = false;
  bool frame_done_ = true;
  bool published_ = false;
};

}  // namespace hearthmap_device
