#include "hearthmap_component.h"

#include <string>

#include "esphome/components/api/api_server.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/theme.h"
#include "hearthmap_gen/generated_config.h"
#include "hearthmap_gen/photos.h"
#include "hearthmap_lib/host/summary.h"
#include "readings_ingest.h"

namespace esphome::hearthmap_panel {

static const char* const TAG = "hearthmap";

namespace {

// Issues shown on screen when the config is refused; the rest go to the log only.
constexpr std::size_t kIssuesOnScreen = 8;

lv_image_dsc_t rgb565_dsc(const uint8_t* pixels, uint32_t w, uint32_t h) {
  lv_image_dsc_t d{};
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_RGB565;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 2U;
  d.data_size = w * h * 2U;
  d.data = pixels;
  return d;
}

bool same_field(const hm::HeatField& a, const hm::HeatField& b) {
  if (a.cols != b.cols || a.rows != b.rows || a.cells.size() != b.cells.size()) return false;
  for (std::size_t i = 0; i < a.cells.size(); ++i) {
    if (a.cells[i].state != b.cells[i].state) return false;
    if (a.cells[i].state == hm::CellState::Value && a.cells[i].value != b.cells[i].value)
      return false;
  }
  return true;
}

}  // namespace

void GeneratedPhotos::init() {
  photos_.reserve(hearthmap_gen::kPhotoCount);
  for (unsigned i = 0; i < hearthmap_gen::kPhotoCount; ++i) {
    const hearthmap_gen::PhotoData& p = hearthmap_gen::kPhotos[i];
    photos_.push_back(rgb565_dsc(p.pixels, p.size, p.size));
  }
}

const lv_image_dsc_t* GeneratedPhotos::plant_photo(std::string_view photo_id) const {
  for (unsigned i = 0; i < hearthmap_gen::kPhotoCount; ++i) {
    if (photo_id == hearthmap_gen::kPhotos[i].id) return &photos_[i];
  }
  return nullptr;
}

void HearthmapComponent::setup() {
  hm::ConfigLoadResult loaded = hearthmap_gen::GeneratedConfigSource{}.load();
  for (const hm::ConfigIssue& issue : loaded.issues) {
    if (issue.severity == hm::IssueSeverity::Error) {
      ESP_LOGE(TAG, "config %s: %s", issue.path.c_str(), issue.message.c_str());
    } else {
      ESP_LOGW(TAG, "config %s: %s", issue.path.c_str(), issue.message.c_str());
    }
  }
  if (!loaded.config.has_value()) {
    // Never render a config core refused: say why, on screen and in the log, and stop.
    std::string text = "hearthmap: the config was refused\n\n";
    std::size_t shown = 0;
    for (const hm::ConfigIssue& issue : loaded.issues) {
      if (issue.severity != hm::IssueSeverity::Error) continue;
      if (shown++ == kIssuesOnScreen) {
        text += "...and more: see the device log\n";
        break;
      }
      text += issue.path + ": " + issue.message + "\n";
    }
    this->phase_ = Phase::ConfigError;
    this->show_status_(text.c_str());
    this->status_set_error(LOG_STR("config refused by validate()"));
    return;
  }
  this->config_ = std::move(*loaded.config);
  this->ui_state_ = hm::initial_ui_state(this->config_);
  if (!this->start_ui_()) return;
  this->subscribe_();
  this->phase_ = Phase::WaitingForTime;
  this->show_status_("Waiting for time");
}

bool HearthmapComponent::start_ui_() {
  hm::build_room_mask(this->config_, this->mask_);
  for (std::size_t i = 0; i < hm::kLayerCount; ++i) {
    hm::build_colour_lut(this->config_.layers[i].band, hm::ui::theme::kHeatPalette,
                         hm::kDefaultColourLutSize, this->luts_[i]);
  }
  this->photos_.init();
  this->raster_ = std::make_unique<hm::ui::HeatRasterizer>(this->config_);
  RAMAllocator<hm::Rgb565> allocator;  // PSRAM when present
  this->heat_pixels_ = allocator.allocate(this->raster_->pixel_count());
  if (this->heat_pixels_ == nullptr) {
    ESP_LOGE(TAG, "no memory for the %ux%u heat image",
             static_cast<unsigned>(this->raster_->width()),
             static_cast<unsigned>(this->raster_->height()));
    this->phase_ = Phase::ConfigError;
    this->show_status_("hearthmap: not enough memory for the heat image");
    this->mark_failed();
    return false;
  }
  std::fill_n(this->heat_pixels_, this->raster_->pixel_count(), hm::Rgb565{0});
  this->heat_dsc_ = rgb565_dsc(reinterpret_cast<const uint8_t*>(this->heat_pixels_),
                               static_cast<uint32_t>(this->raster_->width()),
                               static_cast<uint32_t>(this->raster_->height()));
  hm::ui::ScreenDeps deps;
  deps.fonts = hm::ui::builtin_fonts();
  deps.sink = this;
  deps.photos = &this->photos_;
  this->screen_ = std::make_unique<hm::ui::Screen>(this->lvgl_->get_screen_active(), this->config_,
                                                   &this->heat_dsc_, deps);
  return true;
}

void HearthmapComponent::subscribe_() {
  for (std::size_t i = 0; i < hearthmap_gen::kSubscribedEntityCount; ++i) {
    const char* entity = hearthmap_gen::kSubscribedEntities[i].entity_id;
    // const char* overloads: the ids are static tables, so the API stores pointers, not copies.
    api::global_api_server->subscribe_home_assistant_state(
        entity, nullptr, [this, entity](StringRef state) {
          const hm::Timestamp now =
              this->clock_->now().is_valid() ? this->clock_->timestamp_now() : 0;
          hearthmap_device::apply_state(this->readings_, entity,
                                        std::string_view(state.c_str(), state.size()), now);
          this->dirty_ = true;
        });
    api::global_api_server->subscribe_home_assistant_state(
        entity, "unit_of_measurement", [this, entity](StringRef unit) {
          hearthmap_device::apply_unit(this->readings_, entity,
                                       std::string_view(unit.c_str(), unit.size()));
          this->dirty_ = true;
        });
  }
}

void HearthmapComponent::loop() {
  if (this->phase_ == Phase::ConfigError) return;
  if (!this->clock_->now().is_valid()) {
    if (this->phase_ != Phase::WaitingForTime) {
      this->phase_ = Phase::WaitingForTime;
      this->show_status_("Waiting for time");
    }
    this->pending_.clear();
    return;  // Before time is valid nothing is judged: no "everything is stale" screen.
  }
  const uint32_t ms = millis();
  if (this->phase_ == Phase::WaitingForTime) {
    this->phase_ = Phase::Running;
    this->hide_status_();
    this->dirty_ = true;
  }
  bool touched = false;
  for (const hm::UiEvent& e : this->pending_) {
    this->ui_state_ = hm::apply(this->config_, this->ui_state_, e);
    touched = true;
  }
  this->pending_.clear();
  const uint32_t since = ms - this->last_build_ms_;
  if (touched || (this->dirty_ && since >= this->min_redraw_interval_ms_) ||
      since >= this->refresh_interval_ms_) {
    this->rebuild_(this->clock_->timestamp_now());
    this->last_build_ms_ = ms;
  }
}

void HearthmapComponent::rebuild_(hm::Timestamp now) {
  // R8: confirmed while the HA connection that subscribed to states is up; aging otherwise.
  hearthmap_device::restamp(this->readings_,
                            api::global_api_server->is_connected_with_state_subscription(), now);
  hm::build_view_model(this->config_, this->readings_, now, this->ui_state_, this->view_);

  // The heat image is the expensive part (one pass over every map pixel): redraw it only when
  // the layer or the field actually changed.
  const hm::Layer layer = this->view_.active_layer;
  hm::heat_samples(this->config_, this->readings_, now, layer, this->samples_);
  hm::compute_heat_field(this->config_, this->mask_, this->samples_, this->field_);
  if (!this->heat_valid_ || layer != this->shown_layer_ ||
      !same_field(this->field_, this->shown_field_)) {
    hm::ui::colour_cells(this->field_, this->luts_[hm::index_of(layer)], this->cell_colours_);
    this->raster_->render(this->field_, this->cell_colours_, this->heat_pixels_);
    this->screen_->invalidate_heat();
    std::swap(this->shown_field_, this->field_);
    this->shown_layer_ = layer;
    this->heat_valid_ = true;
  }

  hm::ui::HeaderText header;
  header.clock = this->clock_->now().strftime("%H:%M");
  header.summary = hm::host::house_summary(this->config_, this->readings_, now);
  this->screen_->update(this->view_, header, now);
  this->dirty_ = false;
}

void HearthmapComponent::show_status_(const char* text) {
  if (this->status_ == nullptr) {
    this->status_ = lv_label_create(lv_layer_top());
    lv_obj_set_size(this->status_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(this->status_, lv_color_hex(hm::ui::theme::kBackground), 0);
    lv_obj_set_style_bg_opa(this->status_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(this->status_, lv_color_hex(hm::ui::theme::kTextPrimary), 0);
    lv_obj_set_style_text_font(this->status_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_all(this->status_, 48, 0);
    lv_obj_add_flag(this->status_, LV_OBJ_FLAG_CLICKABLE);  // swallow taps while shown
  }
  lv_label_set_text(this->status_, text);
  lv_obj_remove_flag(this->status_, LV_OBJ_FLAG_HIDDEN);
}

void HearthmapComponent::hide_status_() {
  if (this->status_ != nullptr) lv_obj_add_flag(this->status_, LV_OBJ_FLAG_HIDDEN);
}

void HearthmapComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "hearthmap:\n"
                "  Config: %s\n"
                "  Entities subscribed: %u\n"
                "  Photos: %u\n"
                "  Refresh interval: %u ms\n"
                "  Min redraw interval: %u ms",
                hearthmap_gen::kConfigSourceName,
                static_cast<unsigned>(hearthmap_gen::kSubscribedEntityCount),
                hearthmap_gen::kPhotoCount, static_cast<unsigned>(this->refresh_interval_ms_),
                static_cast<unsigned>(this->min_redraw_interval_ms_));
  if (this->raster_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Heat image: %dx%d (%u bytes), rasterizer working set %u bytes",
                  static_cast<int>(this->raster_->width()),
                  static_cast<int>(this->raster_->height()),
                  static_cast<unsigned>(this->raster_->pixel_count() * 2U),
                  static_cast<unsigned>(this->raster_->working_bytes()));
  }
  if (this->phase_ == Phase::ConfigError)
    ESP_LOGCONFIG(TAG, "  Config REFUSED or out of memory: see errors above");
}

}  // namespace esphome::hearthmap_panel
