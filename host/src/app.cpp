#include "app.h"

#include <utility>

#include "clock_text.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/format.h"
#include "hearthmap/ui/theme.h"

namespace hearthmap::host {

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"  // lv_image_header_t bit-fields
#endif
static void describe_rgb565(lv_image_dsc_t& dsc, const std::vector<Rgb565>& pixels, std::int32_t w,
                            std::int32_t h) {
  dsc = lv_image_dsc_t{};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = static_cast<std::uint32_t>(w);
  dsc.header.h = static_cast<std::uint32_t>(h);
  dsc.header.stride = static_cast<std::uint32_t>(w) * 2U;
  dsc.data_size = static_cast<std::uint32_t>(pixels.size() * sizeof(Rgb565));
  dsc.data = reinterpret_cast<const std::uint8_t*>(pixels.data());
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

App::App(lv_obj_t* parent, Config config, Readings readings, Timestamp now, int utc_offset_minutes,
         const ui::ImageProvider* photos, std::size_t pulse_memory_bytes)
    : config_(std::move(config)),
      readings_(std::move(readings)),
      now_(now),
      utc_offset_minutes_(utc_offset_minutes),
      state_(initial_ui_state(config_)),
      raster_(config_) {
  build_room_mask(config_, mask_);
  for (std::size_t i = 0; i < kLayerCount; ++i) {
    build_colour_lut(config_.layers[i].band, ui::theme::kHeatPalette, kDefaultColourLutSize,
                     luts_[i]);
  }
  heat_pixels_.assign(raster_.pixel_count(), 0);
  describe_rgb565(heat_dsc_, heat_pixels_, raster_.width(), raster_.height());
  ui::ScreenDeps deps;
  deps.fonts = ui::builtin_fonts();
  deps.sink = this;
  deps.photos = photos;
  deps.pulse_memory_bytes = pulse_memory_bytes;
  screen_ = std::make_unique<ui::Screen>(parent, config_, &heat_dsc_, deps);
  // The map bake (walls and labels) before the first image, so that image carries it.
  while (screen_->prepare_step()) {
  }
  refresh();
}

void App::render_heat(Layer layer) {
  heat_samples(config_, readings_, now_, layer, samples_);
  compute_heat_field(config_, mask_, samples_, field_);
  ui::colour_cells(field_, luts_[index_of(layer)], cell_colours_);
  raster_.render(field_, cell_colours_, heat_pixels_.data());
  heat_baked_ = screen_->bake_rows(heat_pixels_.data(), raster_.width(), 0, raster_.height());
  screen_->invalidate_heat();
  heat_layer_ = layer;
}

void App::refresh() {
  build_view_model(config_, readings_, now_, state_, vm_);
  if (heat_layer_ != vm_.active_layer) render_heat(vm_.active_layer);
  ui::HeaderText header{clock_text(now_, utc_offset_minutes_), ui::summary_text(vm_.summary)};
  ui::SourceStatus status;
  status.heat_baked = heat_baked_;
  screen_->update(vm_, header, now_, status);
  // The host has no frame budget: prepare every pulse sprite now (the device slices this).
  while (screen_->prepare_step()) {
  }
}

std::size_t App::process_events() {
  std::vector<UiEvent> events;
  events.swap(pending_);
  for (const UiEvent& e : events) state_ = apply(config_, state_, e);
  if (!events.empty()) refresh();
  return events.size();
}

void App::set_layer(Layer layer) {
  state_ = apply(config_, state_, UiEvent::select_layer(layer));
  refresh();
}

}  // namespace hearthmap::host
