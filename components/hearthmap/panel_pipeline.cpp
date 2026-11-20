#include "panel_pipeline.h"

#include <utility>

namespace hearthmap_device {

namespace hm = ::hearthmap;

namespace {

// Samples are the heat image's whole input besides the config, so equal samples (exactly:
// same order, positions, rooms and float values) draw the same image, and the rebuild can be
// skipped without computing the field at all.
bool same_samples(const std::vector<hm::HeatSample>& a, const std::vector<hm::HeatSample>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].room_index != b[i].room_index || a[i].value != b[i].value ||
        a[i].position.x != b[i].position.x || a[i].position.y != b[i].position.y) {
      return false;
    }
  }
  return true;
}

constexpr std::int32_t kPrepareUnits = static_cast<std::int32_t>(hm::kLayerCount) + 1;

}  // namespace

const char* PanelPipeline::stage_name(Stage stage) {
  switch (stage) {
    case Stage::Build: return "build";
    case Stage::Validate: return "validate";
    case Stage::Prepare: return "prepare";
    case Stage::Mask: return "mask";
    case Stage::Raster: return "raster";
    case Stage::Ready: return "ready";
    case Stage::Refused: return "refused";
  }
  return "?";
}

std::int32_t PanelPipeline::stage_total() const {
  switch (stage_) {
    case Stage::Build: return static_cast<std::int32_t>(build_.steps);
    case Stage::Validate:
      return validator_ ? static_cast<std::int32_t>(validator_->steps_total()) : 0;
    case Stage::Prepare: return kPrepareUnits;
    case Stage::Mask: return mask_.rows;
    case Stage::Raster: return config_.heat.grid_rows + config_.canvas.map_area.height;
    default: return 0;
  }
}

PanelPipeline::Stage PanelPipeline::boot_step(std::int32_t rows) {
  if (rows < 1) rows = 1;
  switch (stage_) {
    case Stage::Build: {
      if (static_cast<std::size_t>(done_) < build_.steps) {
        build_.step(config_, static_cast<std::size_t>(done_));
        ++done_;
      }
      if (static_cast<std::size_t>(done_) >= build_.steps) {
        validator_ = std::make_unique<hm::ConfigValidator>(config_);
        done_ = 0;
        stage_ = Stage::Validate;
      }
      break;
    }
    case Stage::Validate: {
      // One unit of validate() (a room, a pin, ...): the same issues, in the same order, as
      // validate() (core's ConfigValidator), so this is the D-017 check, spread out.
      const bool finished = validator_->step();
      ++done_;
      for (auto& issue : validator_->take_issues()) issues_.push_back(std::move(issue));
      if (finished) {
        validator_.reset();
        done_ = 0;
        stage_ = hm::usable(issues_) ? Stage::Prepare : Stage::Refused;
      }
      break;
    }
    case Stage::Prepare: {
      if (done_ < static_cast<std::int32_t>(hm::kLayerCount)) {
        const auto i = static_cast<std::size_t>(done_);
        hm::build_colour_lut(config_.layers[i].band, palette_, hm::kDefaultColourLutSize, luts_[i]);
        ++done_;
        break;
      }
      mask_.cols = config_.heat.grid_cols;
      mask_.rows = config_.heat.grid_rows;
      mask_.room_index.assign(
          static_cast<std::size_t>(mask_.cols) * static_cast<std::size_t>(mask_.rows),
          hm::RoomMask::kOutside);
      scanner_ = std::make_unique<hm::RoomScanner>(config_);
      done_ = 0;
      stage_ = Stage::Mask;
      break;
    }
    case Stage::Mask: {
      // Exactly build_room_mask(), a row at a time (both are point_in_polygon() at each centre).
      for (std::int32_t n = 0; n < rows && done_ < mask_.rows; ++n, ++done_) {
        scanner_->scan_row(mask_.cols, mask_.rows, done_, &mask_.room_index[mask_.index(0, done_)]);
      }
      if (done_ >= mask_.rows) {
        scanner_.reset();
        raster_ =
            std::make_unique<hm::ui::HeatRasterizer>(config_, hm::ui::HeatRasterizer::Deferred{});
        done_ = 0;
        stage_ = Stage::Raster;
      }
      break;
    }
    case Stage::Raster: {
      const bool ready = raster_->build_rows(config_, rows);
      done_ += rows;
      if (ready) {
        done_ = stage_total();
        stage_ = Stage::Ready;
      }
      break;
    }
    case Stage::Ready:
    case Stage::Refused: break;
  }
  return stage_;
}

void PanelPipeline::start_rebuild(std::uint32_t mono_ms) {
  job_samples_.assign(latest_.begin(), latest_.end());  // reuses storage once warm
  job_layer_ = view_.active_layer;
  field_ok_ = hm::begin_heat_field(mask_, field_);
  field_row_ = 0;
  image_row_ = -1;
  job_active_ = true;
  held_ = false;
  last_start_ms_ = mono_ms;
  started_once_ = true;
  ++rebuilds_;
}

void PanelPipeline::update(const hm::Readings& readings, hm::Timestamp now,
                           const hm::UiState& state, std::uint32_t mono_ms) {
  if (stage_ != Stage::Ready) return;
  hm::build_view_model(config_, readings, now, state, view_);
  const hm::Layer layer = view_.active_layer;
  hm::heat_samples(config_, readings, now, layer, latest_);
  const bool layer_shown = heat_valid_ && shown_layer_ == layer;
  const bool image_current = layer_shown && same_samples(latest_, shown_samples_);
  if (job_active_) {
    // A rebuild for another layer is abandoned: for the shown image if that is current again
    // (a tab tapped away and back), else restarted for the new layer. One for this layer runs
    // on with the samples it started from; changes since are picked up after it lands.
    if (job_layer_ != layer) {
      if (image_current) {
        job_active_ = false;
      } else {
        start_rebuild(mono_ms);
      }
    }
    held_ = false;
    return;
  }
  if (!layer_shown) {
    start_rebuild(mono_ms);  // the first image, or a layer change: never held back
    return;
  }
  held_ = !image_current;  // heat_step() starts it once the interval allows
}

bool PanelPipeline::heat_step(std::int32_t rows, std::uint32_t mono_ms) {
  if (!job_active_) {
    if (!held_ || stage_ != Stage::Ready) return true;
    if (started_once_ && mono_ms - last_start_ms_ < heat_interval_ms_) return true;
    start_rebuild(mono_ms);
  }
  if (rows < 1) rows = 1;
  if (field_ok_ && field_row_ < mask_.rows) {
    const std::int32_t end = field_row_ + rows;
    hm::compute_heat_field_rows(config_, mask_, job_samples_, field_row_, end, field_);
    field_row_ = end < mask_.rows ? end : mask_.rows;
    if (field_row_ < mask_.rows) return false;
  }
  if (image_row_ < 0) {
    if (back_ == nullptr) {
      job_active_ = false;  // nowhere to draw (the embedder never gave buffers)
      return true;
    }
    hm::ui::colour_cells(field_, luts_[hm::index_of(job_layer_)], cell_colours_);
    prepared_ = raster_->prepare(field_, cell_colours_);  // once per image, not per slice
    image_row_ = 0;
    return false;
  }
  const std::int32_t height = raster_->height();
  if (image_row_ < height) {
    const std::int32_t end = image_row_ + rows * kImageRowsPerFieldRow;
    raster_->render_rows(prepared_, field_, cell_colours_, back_, image_row_, end);
    image_row_ = end < height ? end : height;
    if (image_row_ < height) return false;
  }
  std::swap(front_, back_);
  std::swap(shown_samples_, job_samples_);
  shown_layer_ = job_layer_;
  heat_valid_ = true;
  published_ = true;
  job_active_ = false;
  // Samples that moved on while this rebuild ran wait for the interval, like any other change.
  held_ = shown_layer_ == view_.active_layer && !same_samples(latest_, shown_samples_);
  return true;
}

}  // namespace hearthmap_device
