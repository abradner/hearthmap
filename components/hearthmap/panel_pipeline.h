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
//   Validate  core's ConfigValidator: the header, then one room, then one pin per step; its
//             last step also turns the accepted config into the panel's frame
//             (apply_map_rotation(), D-080: at most 64 x 64 vertices and 256 pins, once)
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
//
// A rebuild's units run as a WorkJob on the embedder's worker when it sets one (D-069): on the
// panel a task on the core the main loop does not use, so heat_step() only hands rebuilds over
// and adopts them. Without one they run inline, a unit per heat_step() call, as before.
// Hand-off: from submit until the job is polled back, the rebuild's working state (job_samples_,
// field_, cell_colours_, prepared_, the row counters, job_baked_ and the target buffer
// slots_[job_slot_].px) belongs to the worker; the main loop touches none of it, and starts no
// other rebuild until the job is back. A rebuild that must stop (a layer change, giving way) is
// cancelled and, once back, discarded (it is no longer wanted); a restart waits for it.
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
#include "hearthmap/ui/worker.h"
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
      : build_(std::move(build)), palette_(palette) {
    heat_job_.p = this;
  }
  ~PanelPipeline();
  PanelPipeline(const PanelPipeline&) = delete;
  PanelPipeline& operator=(const PanelPipeline&) = delete;

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

  /// Heat-image buffers of rasterizer().pixel_count() pixels each, owned by the caller (PSRAM
  /// on the device), at least two. front() is the one to show; rebuilds draw into another. With
  /// at least one more buffer than enabled layers, every layer keeps its own image, rebuilt in
  /// the background, and a layer change shows it at once (D-055); with fewer, recently shown
  /// layers stay until their buffer is needed.
  void set_heat_buffers(const std::vector<hearthmap::Rgb565*>& buffers);
  void set_heat_buffers(hearthmap::Rgb565* a, hearthmap::Rgb565* b) {
    set_heat_buffers(std::vector<hearthmap::Rgb565*>{a, b});
  }
  hearthmap::Rgb565* front() const { return slots_.empty() ? nullptr : slots_[front_slot_].px; }
  /// The shown image carries the walls and labels (every row went through the row hook with
  /// success).
  bool front_baked() const { return heat_valid_ && !slots_.empty() && slots_[front_slot_].baked; }
  /// Images currently kept (valid buffers).
  std::size_t cached_layers() const;

  /// Called on rows [begin, end) of an image just rasterised into `image` (stride in pixels);
  /// returns whether it laid the walls and labels over them (D-055).
  using RowHook = std::function<bool(hearthmap::Rgb565* image, std::int32_t stride,
                                     std::int32_t begin, std::int32_t end)>;
  /// With a worker the hook runs on it: `ready` (optional) is asked on the main loop when a
  /// rebuild is handed over, and only if it says true is the hook called for that rebuild; the
  /// embedder keeps everything the hook reads unchanged once `ready` has said true (D-069).
  void set_row_hook(RowHook hook, std::function<bool()> ready = nullptr) {
    row_hook_ = std::move(hook);
    hook_ready_ = std::move(ready);
  }
  /// Where rebuilds run (D-069). nullptr: inline, a unit per heat_step(). Set it before the
  /// first rebuild; it must outlive the pipeline, and its Heat lane is the pipeline's alone.
  void set_worker(hearthmap::ui::Worker* worker) {
    worker_ = worker != nullptr ? worker : &inline_worker_;
  }
  /// A rebuild is wanted or running (on the worker or inline).
  bool heat_in_flight() const { return job_active_ || job_out_ || restart_pending_; }
  /// Redraw every kept image (the walls and labels became available): the shown one at once,
  /// the others in the background.
  void request_rebuild_all();

  /// Minimum time between the starts of two heat rebuilds that only follow changed samples
  /// (the YAML's heat_refresh_interval). The first image and layer changes are never held back.
  void set_heat_refresh_interval_ms(std::uint32_t ms) { heat_interval_ms_ = ms; }

  /// Rebuild the view model for (readings, now, state) and decide on heat work (file comment).
  /// `mono_ms` is a monotonic millisecond clock; it may wrap. Ready only.
  void update(const hearthmap::Readings& readings, hearthmap::Timestamp now,
              const hearthmap::UiState& state, std::uint32_t mono_ms);
  /// One bounded unit of heat work; first starts a held-back rebuild whose interval has passed
  /// by `mono_ms`. Returns true when the main loop has no more heat work now: idle, or (with a
  /// threaded worker) a rebuild is on the worker, so the caller never waits on it.
  bool heat_step(std::int32_t rows, std::uint32_t mono_ms);
  /// True once for each rebuild that swapped in a new front image since the last call.
  bool take_published() {
    const bool p = published_;
    published_ = false;
    return p;
  }
  /// The last publish was a layer change to an image already kept (repaint it at once), not a
  /// fresh image of the shown layer (repaint it in bands).
  bool published_switch() const { return published_switch_; }
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

  struct Slot {
    hearthmap::Rgb565* px = nullptr;
    bool valid = false;
    bool baked = false;
    hearthmap::Layer layer = hearthmap::Layer::Temperature;
    std::vector<hearthmap::HeatSample> samples;  ///< What this image was drawn from.
    std::uint64_t shown_at = 0;
  };
  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);
  std::vector<Slot> slots_;
  std::size_t front_slot_ = 0;
  std::size_t job_slot_ = 0;
  std::uint64_t show_counter_ = 0;
  RowHook row_hook_;
  std::function<bool()> hook_ready_;
  /// One rebuild's units, run by the worker (or inline). See the file comment for what it owns.
  struct HeatJob final : hearthmap::ui::WorkJob {
    PanelPipeline* p = nullptr;
    std::int32_t rows = 1;
    bool use_hook = false;
    hearthmap::Rgb565* back = nullptr;
    const hearthmap::ColourLut* lut = nullptr;
    bool step() override;
  };
  HeatJob heat_job_;
  hearthmap::ui::InlineWorker inline_worker_;
  hearthmap::ui::Worker* worker_ = &inline_worker_;
  bool job_out_ = false;          ///< the worker holds the rebuild's state
  bool restart_pending_ = false;  ///< a rebuild waits for the cancelled one to come back
  hearthmap::Layer restart_layer_ = hearthmap::Layer::Temperature;
  /// start_rebuild() now, or once the job on the worker is back (it is cancelled).
  void request_rebuild(hearthmap::Layer layer, std::uint32_t mono_ms);
  /// The rebuild in flight is not wanted any more: cancelled (and reclaimed when it can be).
  void abandon_rebuild();
  /// Hands the rebuild to the worker, then polls it once (inline: one unit runs).
  bool run_rebuild(std::int32_t rows);
  /// The worker's answer for the rebuild: true when the main loop has nothing more to do now.
  bool settle_rebuild(hearthmap::ui::Worker::Poll poll, bool completed);
  void finish_rebuild();
  bool job_baked_ = false;
  bool force_ = false;
  /// request_rebuild_all() asked for the walls; an image published without them since (it was
  /// handed over before the bake was ready) is drawn again (finish_rebuild()).
  bool bake_pending_ = false;
  bool published_switch_ = false;
  void start_rebuild(hearthmap::Layer layer, std::uint32_t mono_ms);
  std::size_t slot_for(hearthmap::Layer layer) const;
  std::size_t pick_target(hearthmap::Layer layer) const;
  bool background_enabled() const;
  void show_slot(std::size_t i, bool layer_switch);

  hearthmap::ViewModel view_;
  /// Samples per layer, from update() (the shown layer always; others when images are kept).
  std::array<std::vector<hearthmap::HeatSample>, hearthmap::kLayerCount> latest_by_layer_;
  std::vector<hearthmap::HeatSample> job_samples_;  ///< What the rebuild in flight draws.
  hearthmap::HeatField field_;
  bool heat_valid_ = false;  ///< A front image exists.
  hearthmap::Layer shown_layer_ = hearthmap::Layer::Temperature;
  hearthmap::Layer job_layer_ = hearthmap::Layer::Temperature;
  hearthmap::ui::BulkVector<hearthmap::Rgb565> cell_colours_;  ///< the memory port (D-064)
  hearthmap::ui::HeatRasterizer::Prepared prepared_;           ///< For cell_colours_, per image.
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
