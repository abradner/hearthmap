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

PanelPipeline::~PanelPipeline() {
  // A rebuild on the worker writes this pipeline's buffers: it comes back first (at most one unit
  // later). Only at destruction, never in the running loop.
  if (job_out_) {
    worker_->cancel(hearthmap::ui::WorkLane::Heat);
    while (worker_->poll(hearthmap::ui::WorkLane::Heat) == hearthmap::ui::Worker::Poll::Busy) {
    }
  }
}

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
        // The stepped boot never calls load(), so it hands over the panel's frame itself, as
        // every ConfigSource does (core's config.h, D-080): one pass over vertices and pins.
        if (stage_ == Stage::Prepare) hm::apply_map_rotation(config_);
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

void PanelPipeline::set_heat_buffers(const std::vector<hm::Rgb565*>& buffers) {
  slots_.clear();
  for (hm::Rgb565* px : buffers) {
    if (px == nullptr) continue;
    Slot slot;
    slot.px = px;
    slots_.push_back(std::move(slot));
  }
  front_slot_ = 0;
  job_slot_ = slots_.size() > 1 ? 1 : 0;
  heat_valid_ = false;
}

std::size_t PanelPipeline::cached_layers() const {
  std::size_t n = 0;
  for (const Slot& s : slots_) n += s.valid ? 1U : 0U;
  return n;
}

std::size_t PanelPipeline::slot_for(hm::Layer layer) const {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].valid && slots_[i].layer == layer) return i;
  }
  return kNoSlot;
}

bool PanelPipeline::background_enabled() const {
  // Each enabled layer can keep its own image and one buffer remains to draw into.
  return slots_.size() >= view_.tabs.size() + 1;
}

void PanelPipeline::show_slot(std::size_t i, bool layer_switch) {
  front_slot_ = i;
  shown_layer_ = slots_[i].layer;
  heat_valid_ = true;
  published_ = true;
  published_switch_ = layer_switch;
  slots_[i].shown_at = ++show_counter_;
}

std::size_t PanelPipeline::pick_target(hm::Layer layer) const {
  // Into a free buffer if there is one; else over this layer's own stale copy; else over the
  // least recently shown other layer. Never into front(), shown or not: the embedder may be
  // displaying it already.
  const auto shown = [this](std::size_t i) { return i == front_slot_; };
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!shown(i) && !slots_[i].valid) return i;
  }
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!shown(i) && slots_[i].layer == layer) return i;
  }
  std::size_t best = kNoSlot;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (shown(i)) continue;
    if (best == kNoSlot || slots_[i].shown_at < slots_[best].shown_at) best = i;
  }
  return best;
}

void PanelPipeline::start_rebuild(hm::Layer layer, std::uint32_t mono_ms) {
  const std::size_t target = pick_target(layer);
  if (target == kNoSlot) return;
  job_slot_ = target;
  slots_[target].valid = false;  // being drawn over
  const auto& latest = latest_by_layer_[hm::index_of(layer)];
  job_samples_.assign(latest.begin(), latest.end());  // reuses storage once warm
  job_layer_ = layer;
  job_baked_ = true;
  field_ok_ = hm::begin_heat_field(mask_, field_);
  // Sized here, on the main loop, so nothing in the job allocates (D-069).
  cell_colours_.resize(field_.cells.size());
  field_row_ = 0;
  image_row_ = -1;
  job_active_ = true;
  if (layer == view_.active_layer) {
    held_ = false;
    force_ = false;
  }
  last_start_ms_ = mono_ms;
  started_once_ = true;
  ++rebuilds_;
}

void PanelPipeline::update(const hm::Readings& readings, hm::Timestamp now,
                           const hm::UiState& state, std::uint32_t mono_ms) {
  if (stage_ != Stage::Ready) return;
  hm::build_view_model(config_, readings, now, state, view_);
  const hm::Layer layer = view_.active_layer;
  // Samples for the shown layer, and for every enabled layer when their images are kept.
  if (background_enabled()) {
    for (hm::Layer l : view_.tabs) {
      if (l != layer)
        hm::heat_samples(config_, readings, now, l, latest_by_layer_[hm::index_of(l)]);
    }
  }
  hm::heat_samples(config_, readings, now, layer, latest_by_layer_[hm::index_of(layer)]);
  const std::vector<hm::HeatSample>& latest = latest_by_layer_[hm::index_of(layer)];

  // A layer change to an image already drawn shows it at once (D-055).
  if (!(heat_valid_ && shown_layer_ == layer)) {
    const std::size_t cached = slot_for(layer);
    if (cached != kNoSlot) show_slot(cached, true);
  }
  const bool layer_shown = heat_valid_ && shown_layer_ == layer;
  const bool image_current =
      layer_shown && !force_ && same_samples(latest, slots_[front_slot_].samples);
  if (job_active_) {
    // A rebuild for another layer runs on while this layer's image is up and current (a
    // background build, or a tab tapped away and back to a kept image): views come every few
    // hundred ms, faster than a rebuild on the panel, so stopping it here would restart it
    // forever. It gives way when this layer needs a new image (its buffer stays free; background
    // work resumes later), and restarts for this layer when nothing of it is shown. One for this
    // layer runs on with the samples it started from; changes since are picked up after it lands.
    if (job_layer_ != layer) {
      if (!layer_shown) {
        request_rebuild(layer, mono_ms);
      } else if (!image_current || job_slot_ == front_slot_) {
        abandon_rebuild();
      }
    }
    held_ = layer_shown && !image_current;
    return;
  }
  if (!layer_shown) {
    request_rebuild(layer, mono_ms);  // the first image, or a layer never kept: never held back
    return;
  }
  held_ = !image_current;  // heat_step() starts it once the interval allows
}

void PanelPipeline::request_rebuild_all() {
  // Every kept image is redrawn: the shown one at once (it stays up meanwhile), the others in
  // the background as they come up.
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!(heat_valid_ && i == front_slot_)) slots_[i].valid = false;
  }
  force_ = heat_valid_;
  // A rebuild in flight now (the first image, before anything is shown, or a background one) was
  // handed over without the walls: it must not be the last word (finish_rebuild()).
  bake_pending_ = true;
}

void PanelPipeline::request_rebuild(hm::Layer layer, std::uint32_t mono_ms) {
  if (job_out_) {
    worker_->cancel(hearthmap::ui::WorkLane::Heat);
    if (worker_->poll(hearthmap::ui::WorkLane::Heat) != hearthmap::ui::Worker::Poll::Finished) {
      // Still on the worker: it is stopped between two units, and heat_step() starts this one
      // once it is back (its state is the worker's until then).
      job_active_ = false;
      restart_pending_ = true;
      restart_layer_ = layer;
      return;
    }
    job_out_ = false;
  }
  restart_pending_ = false;
  start_rebuild(layer, mono_ms);
}

void PanelPipeline::abandon_rebuild() {
  job_active_ = false;
  restart_pending_ = false;
  if (!job_out_) return;
  worker_->cancel(hearthmap::ui::WorkLane::Heat);
  // Inline the cancellation runs now; a threaded worker gives it back to a later heat_step().
  if (worker_->poll(hearthmap::ui::WorkLane::Heat) == hearthmap::ui::Worker::Poll::Finished) {
    job_out_ = false;
  }
}

bool PanelPipeline::HeatJob::step() {
  PanelPipeline& q = *p;
  if (q.field_ok_ && q.field_row_ < q.mask_.rows) {
    const std::int32_t end = q.field_row_ + rows;
    hm::compute_heat_field_rows(q.config_, q.mask_, q.job_samples_, q.field_row_, end, q.field_);
    q.field_row_ = end < q.mask_.rows ? end : q.mask_.rows;
    if (q.field_row_ < q.mask_.rows) return true;
  }
  if (q.image_row_ < 0) {
    hm::ui::colour_cells(q.field_, *lut, q.cell_colours_);
    q.prepared_ = q.raster_->prepare(q.field_, q.cell_colours_);  // once per image, not per slice
    q.image_row_ = 0;
    return true;
  }
  const std::int32_t height = q.raster_->height();
  if (q.image_row_ < height) {
    const std::int32_t end = q.image_row_ + rows * kImageRowsPerFieldRow;
    const std::int32_t stop = end < height ? end : height;
    q.raster_->render_rows(q.prepared_, q.field_, q.cell_colours_, back, q.image_row_, stop);
    const bool baked = use_hook ? q.row_hook_(back, q.raster_->width(), q.image_row_, stop) : false;
    q.job_baked_ = q.job_baked_ && baked;
    q.image_row_ = stop;
  }
  return q.image_row_ < height;
}

bool PanelPipeline::run_rebuild(std::int32_t rows) {
  if (!job_out_) {
    heat_job_.rows = rows < 1 ? 1 : rows;
    heat_job_.back = slots_[job_slot_].px;
    heat_job_.lut = &luts_[hm::index_of(job_layer_)];
    // Asked here, on the main loop: from now on the hook reads only what `ready` vouched for.
    heat_job_.use_hook = row_hook_ && (!hook_ready_ || hook_ready_());
    job_out_ = worker_->submit(hearthmap::ui::WorkLane::Heat, heat_job_);
    if (!job_out_) return true;  // the lane is someone else's: nothing to run (a wiring error)
  }
  bool completed = false;
  const hearthmap::ui::Worker::Poll poll = worker_->poll(hearthmap::ui::WorkLane::Heat, &completed);
  return settle_rebuild(poll, completed);
}

bool PanelPipeline::settle_rebuild(hearthmap::ui::Worker::Poll poll, bool completed) {
  // Busy: inline, more units remain for the next call; threaded, the worker has it and the main
  // loop has nothing to wait for.
  if (poll == hearthmap::ui::Worker::Poll::Busy) return worker_->threaded();
  job_out_ = false;
  // No other rebuild can have started while this one was out (start_rebuild() waits for it to
  // come back), so the rebuild that comes back is the one in flight: it is adopted when it ran
  // to its end and is still wanted (job_active_), and discarded when it was cancelled or given
  // up meanwhile. No generation counter is needed for that.
  if (completed && job_active_) {
    finish_rebuild();
    return true;
  }
  return !job_active_ && !restart_pending_;
}

bool PanelPipeline::heat_step(std::int32_t rows, std::uint32_t mono_ms) {
  if (job_out_) {
    bool completed = false;
    const hearthmap::ui::Worker::Poll poll =
        worker_->poll(hearthmap::ui::WorkLane::Heat, &completed);
    const bool done = settle_rebuild(poll, completed);
    if (job_out_) return done;  // still on the worker (or, inline, units remain)
    // Back: adopted (finish_rebuild() ended it) or discarded. A rebuild that waited for it
    // starts now; otherwise this call is done.
    if (restart_pending_) {
      restart_pending_ = false;
      start_rebuild(restart_layer_, mono_ms);
    }
    if (!job_active_) return true;
  }
  if (!job_active_) {
    if (stage_ != Stage::Ready || slots_.size() < 2) return true;
    const bool interval_ok = !started_once_ || mono_ms - last_start_ms_ >= heat_interval_ms_;
    if (force_ || (held_ && interval_ok)) {
      start_rebuild(view_.active_layer, mono_ms);
    } else if (!held_ && background_enabled() && interval_ok) {
      // Low priority: a layer not shown whose image is missing or behind its samples.
      for (hm::Layer l : view_.tabs) {
        if (l == view_.active_layer) continue;
        const std::size_t s = slot_for(l);
        if (s != kNoSlot && same_samples(latest_by_layer_[hm::index_of(l)], slots_[s].samples)) {
          continue;
        }
        start_rebuild(l, mono_ms);
        break;
      }
    }
    if (!job_active_) return true;
  }
  return run_rebuild(rows);
}

void PanelPipeline::finish_rebuild() {
  Slot& done = slots_[job_slot_];
  done.valid = true;
  done.layer = job_layer_;
  done.baked = job_baked_;
  std::swap(done.samples, job_samples_);
  job_active_ = false;
  // An older copy of the same layer is now free, unless it is on screen (another layer's job
  // never replaces what is shown; the shown layer's own job replaces it below).
  const bool for_shown = job_layer_ == view_.active_layer;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (i == job_slot_ || !slots_[i].valid || slots_[i].layer != job_layer_) continue;
    if (heat_valid_ && i == front_slot_ && !for_shown) continue;
    slots_[i].valid = false;
  }
  if (for_shown) show_slot(job_slot_, false);
  // Drawn without the walls although they were asked for (the rebuild was handed over before the
  // bake was ready): shown, it is redrawn once more; in the background, it is dropped and comes
  // up again. Asked on the main loop; with the hook ready, the next rebuild carries the walls.
  if (bake_pending_ && !job_baked_) {
    const bool hook_now = row_hook_ && (!hook_ready_ || hook_ready_());
    if (hook_now && for_shown) {
      force_ = true;
      bake_pending_ = false;  // one retry: the redraw starts with the hook
    } else if (hook_now) {
      done.valid = false;
    }
  } else if (for_shown && job_baked_) {
    bake_pending_ = false;
  }
  // Samples that moved on while this rebuild ran wait for the interval, like any other change.
  held_ = heat_valid_ && shown_layer_ == view_.active_layer &&
          !same_samples(latest_by_layer_[hm::index_of(view_.active_layer)],
                        slots_[front_slot_].samples);
}

}  // namespace hearthmap_device
