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
// Running, two independent paths (docs/decisions.md D-047):
//   update()     the view model, at once (core's build_view_model(), reuse form), and the heat
//                samples for the layer it shows. Cheap: the embedder calls it whenever readings,
//                time or UI state change, so taps reach the screen within one loop slice.
//                It also decides whether the heat image needs a rebuild (below).
//   heat_step()  one bounded unit of a heat rebuild: `rows` heat-field rows, then the cell
//                colours, then `rows` x kImageRowsPerFieldRow image rows into the BACK buffer.
//                When the rebuild completes the buffers swap, so the image the screen shows is
//                never half written (take_published() says it happened).
// A rebuild starts at once for the first image and for a layer change (restarting one in flight
// for another layer). A rebuild for changed samples on the same layer starts at most once per
// heat refresh interval; until then the image lags the pins, which always show the latest
// values. Unchanged samples on the shown layer never rebuild.
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

  /// Minimum time between the starts of two heat rebuilds that only follow changed samples
  /// (the YAML's heat_refresh_interval). The first image and layer changes are never held back.
  void set_heat_refresh_interval_ms(std::uint32_t ms) { heat_interval_ms_ = ms; }

  /// Rebuild the view model for (readings, now, state) and decide on heat work (file comment).
  /// `mono_ms` is a monotonic millisecond clock; it may wrap. Ready only.
  void update(const hearthmap::Readings& readings, hearthmap::Timestamp now,
              const hearthmap::UiState& state, std::uint32_t mono_ms);
  /// One bounded unit of heat work; first starts a held-back rebuild whose interval has passed
  /// by `mono_ms`. Returns true when no heat work is left (idle).
  bool heat_step(std::int32_t rows, std::uint32_t mono_ms);
  /// True once for each rebuild that swapped in a new front image since the last call.
  bool take_published() {
    const bool p = published_;
    published_ = false;
    return p;
  }
  /// The front image does not show the view model's layer (no image yet, or a rebuild for a
  /// newly selected layer is in flight): the screen dims the map meanwhile (D-047).
  bool heat_behind() const { return !heat_valid_ || shown_layer_ != view_.active_layer; }
  /// Rebuilds started since boot (for tests and logs).
  std::uint32_t heat_rebuilds() const { return rebuilds_; }
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
  void start_rebuild(std::uint32_t mono_ms);

  hearthmap::ViewModel view_;
  std::vector<hearthmap::HeatSample> latest_;         ///< Samples for view_'s layer, from update().
  std::vector<hearthmap::HeatSample> job_samples_;    ///< What the rebuild in flight draws.
  std::vector<hearthmap::HeatSample> shown_samples_;  ///< What the front image was drawn from.
  hearthmap::HeatField field_;
  bool heat_valid_ = false;  ///< A front image exists.
  hearthmap::Layer shown_layer_ = hearthmap::Layer::Temperature;
  hearthmap::Layer job_layer_ = hearthmap::Layer::Temperature;
  std::vector<hearthmap::Rgb565> cell_colours_;
  hearthmap::ui::HeatRasterizer::Prepared prepared_;  ///< For cell_colours_, per image.
  std::uint32_t heat_interval_ms_ = 15000;
  std::uint32_t last_start_ms_ = 0;
  bool started_once_ = false;
  bool held_ = false;  ///< Samples changed on the shown layer; rebuild when the interval allows.
  bool job_active_ = false;
  std::int32_t field_row_ = 0;
  std::int32_t image_row_ = -1;  ///< -1: field not finished; then back-buffer rows done.
  bool field_ok_ = false;
  bool published_ = false;
  std::uint32_t rebuilds_ = 0;
};

}  // namespace hearthmap_device
