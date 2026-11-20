#include "hearthmap_component.h"

#include <algorithm>
#include <string>

#include "esphome/components/api/api_server.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/theme.h"
#include "hearthmap_gen/photos.h"
#include "hearthmap_lib/host/summary.h"
#include "readings_ingest.h"

namespace esphome::hearthmap_panel {

static const char* const TAG = "hearthmap";

namespace {

// Work per loop() call: steps run until this much time has gone, then loop() returns so the
// watchdog, Wi-Fi, the API and LVGL get the CPU. One step is a few rows (core's RoomScanner,
// the rasterizer's build, heat-field rows), small enough that a slice overruns the budget by at
// most one step even at the schema's maximums (components/hearthmap/README.md, "Cost").
constexpr uint32_t kSliceBudgetMs = 20;
constexpr std::int32_t kBootRowsPerStep = 4;
constexpr std::int32_t kFieldRowsPerStep = 1;

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

}  // namespace

void HearthmapComponent::setup() {
  // Nothing heavy here: boot work runs in slices from loop(). Subscriptions must exist before
  // Home Assistant connects (it asks for the list once per connection), so they are made now;
  // states that arrive early simply wait in readings_.
  this->boot_started_ms_ = millis();
  this->stage_started_ms_ = this->boot_started_ms_;
  this->pipeline_ =
      std::make_unique<hearthmap_device::PanelPipeline>(this->source_, hm::ui::theme::kHeatPalette);
  this->subscribe_();
  this->show_status_("Starting");
  ESP_LOGI(TAG, "boot: load started (config %s, %u entities)", hearthmap_gen::kConfigSourceName,
           static_cast<unsigned>(hearthmap_gen::kSubscribedEntityCount));
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
  switch (this->phase_) {
    case Phase::Booting: this->boot_slice_(); return;
    case Phase::Failed: return;
    case Phase::WaitingForTime:
    case Phase::Running: break;
  }
  if (!this->clock_->now().is_valid()) {
    if (this->phase_ != Phase::WaitingForTime) {
      this->phase_ = Phase::WaitingForTime;
      this->show_status_("Waiting for time");
    }
    this->pending_.clear();
    return;  // Before time is valid nothing is judged: no "everything is stale" screen.
  }
  if (this->phase_ == Phase::WaitingForTime) {
    ESP_LOGI(TAG, "time is valid: drawing");
    this->phase_ = Phase::Running;
    this->hide_status_();
    this->dirty_ = true;
  }
  this->frame_slice_();
}

void HearthmapComponent::boot_slice_() {
  using Stage = hearthmap_device::PanelPipeline::Stage;
  const uint32_t start = millis();
  ++this->stage_slices_;
  do {
    const Stage before = this->pipeline_->stage();
    const Stage after = this->pipeline_->boot_step(kBootRowsPerStep);
    if (after != before) {
      const uint32_t now = millis();
      ESP_LOGI(TAG, "boot: %s done in %u ms over %u loop slices",
               hearthmap_device::PanelPipeline::stage_name(before),
               static_cast<unsigned>(now - this->stage_started_ms_),
               static_cast<unsigned>(this->stage_slices_));
      this->stage_started_ms_ = now;
      this->stage_slices_ = 0;
      if (after == Stage::Mask || after == Stage::Raster) {
        ESP_LOGI(TAG, "boot: %s started (%d rows)",
                 hearthmap_device::PanelPipeline::stage_name(after),
                 static_cast<int>(this->pipeline_->stage_rows_total()));
      }
    }
    if (after == Stage::Refused) {
      std::string text = "hearthmap: the config was refused\n\n";
      std::size_t shown = 0;
      for (const hm::ConfigIssue& issue : this->pipeline_->issues()) {
        if (issue.severity == hm::IssueSeverity::Error) {
          ESP_LOGE(TAG, "config %s: %s", issue.path.c_str(), issue.message.c_str());
          if (shown++ < kIssuesOnScreen) text += issue.path + ": " + issue.message + "\n";
        } else {
          ESP_LOGW(TAG, "config %s: %s", issue.path.c_str(), issue.message.c_str());
        }
      }
      if (shown > kIssuesOnScreen) text += "...and more: see the device log\n";
      // Never render a config core refused: say why, on screen and in the log, and stop.
      this->phase_ = Phase::Failed;
      this->show_status_(text.c_str());
      this->status_set_error(LOG_STR("config refused by validate()"));
      return;
    }
    if (after == Stage::Ready) {
      for (const hm::ConfigIssue& issue : this->pipeline_->issues()) {
        ESP_LOGW(TAG, "config %s: %s", issue.path.c_str(), issue.message.c_str());
      }
      if (!this->start_screen_()) return;
      ESP_LOGI(TAG, "boot: complete in %u ms",
               static_cast<unsigned>(millis() - this->boot_started_ms_));
      this->ui_state_ = hm::initial_ui_state(this->pipeline_->config());
      this->phase_ = Phase::WaitingForTime;
      this->show_status_("Waiting for time");
      return;
    }
  } while (millis() - start < kSliceBudgetMs);
}

bool HearthmapComponent::start_screen_() {
  const uint32_t start = millis();
  ESP_LOGI(TAG, "boot: screen started");
  const hm::ui::HeatRasterizer& raster = this->pipeline_->rasterizer();
  RAMAllocator<hm::Rgb565> allocator;  // PSRAM when present
  this->heat_pixels_ = allocator.allocate(raster.pixel_count());
  if (this->heat_pixels_ == nullptr) {
    ESP_LOGE(TAG, "no memory for the %dx%d heat image", static_cast<int>(raster.width()),
             static_cast<int>(raster.height()));
    this->phase_ = Phase::Failed;
    this->show_status_("hearthmap: not enough memory for the heat image");
    this->status_set_error(LOG_STR("no memory for the heat image"));
    return false;
  }
  std::fill_n(this->heat_pixels_, raster.pixel_count(), hm::Rgb565{0});
  this->pipeline_->set_heat_buffer(this->heat_pixels_);
  this->heat_dsc_ =
      rgb565_dsc(reinterpret_cast<const uint8_t*>(this->heat_pixels_),
                 static_cast<uint32_t>(raster.width()), static_cast<uint32_t>(raster.height()));
  this->photos_.init();
  hm::ui::ScreenDeps deps;
  deps.fonts = hm::ui::builtin_fonts();
  deps.sink = this;
  deps.photos = &this->photos_;
  this->screen_ = std::make_unique<hm::ui::Screen>(
      this->lvgl_->get_screen_active(), this->pipeline_->config(), &this->heat_dsc_, deps);
  ESP_LOGI(TAG, "boot: screen done in %u ms (heat image %dx%d, %u bytes; rasterizer %u bytes)",
           static_cast<unsigned>(millis() - start), static_cast<int>(raster.width()),
           static_cast<int>(raster.height()), static_cast<unsigned>(raster.pixel_count() * 2U),
           static_cast<unsigned>(raster.working_bytes()));
  return true;
}

void HearthmapComponent::frame_slice_() {
  const uint32_t start = millis();
  if (!this->frame_active_) {
    const bool touched = !this->pending_.empty();
    for (const hm::UiEvent& e : this->pending_) {
      this->ui_state_ = hm::apply(this->pipeline_->config(), this->ui_state_, e);
    }
    this->pending_.clear();
    const uint32_t since = start - this->last_frame_ms_;
    const bool due = !this->first_frame_logged_ || touched || since >= this->refresh_interval_ms_ ||
                     (this->dirty_ && since >= this->min_redraw_interval_ms_);
    if (!due) return;
    const hm::Timestamp now = this->clock_->timestamp_now();
    // R8: confirmed while the HA connection that subscribed to states is up; aging otherwise.
    hearthmap_device::restamp(this->readings_,
                              api::global_api_server->is_connected_with_state_subscription(), now);
    this->pipeline_->begin_frame(this->readings_, now, this->ui_state_);
    this->dirty_ = false;
    this->frame_active_ = true;
    this->frame_started_ms_ = start;
    this->last_frame_ms_ = start;
    if (!this->first_frame_logged_) ESP_LOGI(TAG, "first frame started");
  }
  bool done = false;
  do {
    done = this->pipeline_->frame_step(kFieldRowsPerStep);
  } while (!done && millis() - start < kSliceBudgetMs);
  if (!done) return;

  const hm::Timestamp now = this->clock_->timestamp_now();
  if (this->pipeline_->heat_redrawn()) this->screen_->invalidate_heat();
  hm::ui::HeaderText header;
  header.clock = this->clock_->now().strftime("%H:%M");
  header.summary = hm::host::house_summary(this->pipeline_->config(), this->readings_, now);
  this->screen_->update(this->pipeline_->view(), header, now);
  this->frame_active_ = false;
  const uint32_t took = millis() - this->frame_started_ms_;
  if (!this->first_frame_logged_) {
    ESP_LOGI(TAG, "first frame done in %u ms", static_cast<unsigned>(took));
    this->first_frame_logged_ = true;
  } else {
    ESP_LOGD(TAG, "frame done in %u ms (heat image %s)", static_cast<unsigned>(took),
             this->pipeline_->heat_redrawn() ? "redrawn" : "unchanged");
  }
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
    lv_obj_set_clickable(this->status_, true);  // swallow taps while shown
  }
  lv_label_set_text(this->status_, text);
  lv_obj_set_hidden(this->status_, false);
}

void HearthmapComponent::hide_status_() {
  if (this->status_ != nullptr) lv_obj_set_hidden(this->status_, true);
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
  if (this->phase_ == Phase::Failed)
    ESP_LOGCONFIG(TAG, "  FAILED: config refused or out of memory, see errors above");
}

}  // namespace esphome::hearthmap_panel
