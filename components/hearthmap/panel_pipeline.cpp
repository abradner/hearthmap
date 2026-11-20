#include "panel_pipeline.h"

#include <utility>

namespace hearthmap_device {

namespace hm = ::hearthmap;

namespace {

bool same_field(const hm::HeatField& a, const hm::HeatField& b) {
  if (a.cols != b.cols || a.rows != b.rows || a.cells.size() != b.cells.size()) return false;
  for (std::size_t i = 0; i < a.cells.size(); ++i) {
    if (a.cells[i].state != b.cells[i].state) return false;
    if (a.cells[i].state == hm::CellState::Value && a.cells[i].value != b.cells[i].value) {
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

void PanelPipeline::begin_frame(const hm::Readings& readings, hm::Timestamp now,
                                const hm::UiState& state) {
  if (stage_ != Stage::Ready) return;
  hm::build_view_model(config_, readings, now, state, view_);
  hm::heat_samples(config_, readings, now, view_.active_layer, samples_);
  field_ok_ = hm::begin_heat_field(mask_, field_);
  field_row_ = 0;
  image_row_ = -1;
  frame_done_ = false;
  published_ = false;
}

bool PanelPipeline::frame_step(std::int32_t rows) {
  if (frame_done_) return true;
  if (rows < 1) rows = 1;
  if (field_ok_ && field_row_ < mask_.rows) {
    const std::int32_t end = field_row_ + rows;
    hm::compute_heat_field_rows(config_, mask_, samples_, field_row_, end, field_);
    field_row_ = end < mask_.rows ? end : mask_.rows;
    if (field_row_ < mask_.rows) return false;
  }
  // The heat image is one pass over every map pixel: redraw it only when the layer or the field
  // actually changed. It is drawn into the back buffer, a slice per step, and swapped in whole
  // at the end, so a repaint between slices (a pulse animation over the map) never shows a
  // half-written image under the previous view model.
  const hm::Layer layer = view_.active_layer;
  if (image_row_ < 0) {
    if (back_ == nullptr ||
        (heat_valid_ && layer == shown_layer_ && same_field(field_, shown_field_))) {
      frame_done_ = true;
      return true;
    }
    hm::ui::colour_cells(field_, luts_[hm::index_of(layer)], cell_colours_);
    image_row_ = 0;
    return false;
  }
  const std::int32_t height = raster_->height();
  if (image_row_ < height) {
    const std::int32_t end = image_row_ + rows * kImageRowsPerFieldRow;
    raster_->render_rows(field_, cell_colours_, back_, image_row_, end);
    image_row_ = end < height ? end : height;
    if (image_row_ < height) return false;
  }
  std::swap(front_, back_);
  std::swap(shown_field_, field_);
  shown_layer_ = layer;
  heat_valid_ = true;
  published_ = true;
  frame_done_ = true;
  return true;
}

}  // namespace hearthmap_device
