#include "hearthmap_component.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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
// After a new heat image is swapped in, the map is invalidated in bands of this many rows, one
// band per loop() call at most every kBandIntervalMs, so LVGL repaints it over several refreshes
// instead of in one long one (a full 920 x 736 repaint took 645 ms on the panel).
constexpr int32_t kHeatBandRows = 48;
constexpr uint32_t kBandIntervalMs = 25;
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

namespace {

uint32_t g_lvgl_warnings = 0;
uint32_t g_lvgl_errors = 0;

}  // namespace

uint32_t lvgl_warnings() {
  return g_lvgl_warnings;
}
uint32_t lvgl_errors() {
  return g_lvgl_errors;
}

void hook_lvgl_log() {
#if LV_USE_LOG
  // Replaces ESPHome's own LVGL print callback (same output, tag "lvgl") to count warnings and
  // errors for the frame log, so a run shows whether LVGL complains (D-039). Runs after the
  // lvgl component's setup, which registered the callback this one takes over from.
  lv_log_register_print_cb([](lv_log_level_t level, const char* buf) {
    if (level == LV_LOG_LEVEL_WARN) ++g_lvgl_warnings;
    if (level == LV_LOG_LEVEL_ERROR) ++g_lvgl_errors;
    const char* next = strchr(buf, ')');
    if (next != nullptr) buf = next + 1;
    while (isspace(static_cast<unsigned char>(*buf))) buf++;
    int len = static_cast<int>(strlen(buf));
    if (len > 0 && buf[len - 1] == '\n') --len;
    switch (level) {
      case LV_LOG_LEVEL_TRACE: ESP_LOGD("lvgl", "%.*s", len, buf); break;
      case LV_LOG_LEVEL_INFO: ESP_LOGI("lvgl", "%.*s", len, buf); break;
      case LV_LOG_LEVEL_WARN: ESP_LOGW("lvgl", "%.*s", len, buf); break;
      default: ESP_LOGE("lvgl", "%.*s", len, buf); break;
    }
  });
#endif
}

void HearthmapComponent::setup() {
  // Nothing heavy here: boot work runs in slices from loop(). Subscriptions must exist before
  // Home Assistant connects (it asks for the list once per connection), so they are made now;
  // states that arrive early simply wait in readings_.
  this->boot_started_ms_ = millis();
  this->stage_started_ms_ = this->boot_started_ms_;
  hearthmap_device::PanelPipeline::Builder builder;
  builder.steps = this->source_.build_steps();
  builder.step = [this](hm::Config& c, std::size_t k) { this->source_.build_step(c, k); };
  this->pipeline_ = std::make_unique<hearthmap_device::PanelPipeline>(std::move(builder),
                                                                      hm::ui::theme::kHeatPalette);
  this->pipeline_->set_heat_refresh_interval_ms(this->heat_refresh_interval_ms_);
  hook_lvgl_log();  // after the lvgl component's setup, whose callback this takes over
  this->subscribe_();
  this->show_status_("Starting");
  ESP_LOGI(TAG, "boot: build started (config %s, %u entities)", hearthmap_gen::kConfigSourceName,
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
    const uint32_t step_start = micros();
    const Stage after = this->pipeline_->boot_step(kBootRowsPerStep);
    const uint32_t step_us = micros() - step_start;
    if (step_us > this->stage_worst_us_) this->stage_worst_us_ = step_us;
    if (after != before) {
      const uint32_t now = millis();
      ESP_LOGI(TAG, "boot: %s done in %u ms over %u loop slices (longest step %u.%03u ms)",
               hearthmap_device::PanelPipeline::stage_name(before),
               static_cast<unsigned>(now - this->stage_started_ms_),
               static_cast<unsigned>(this->stage_slices_),
               static_cast<unsigned>(this->stage_worst_us_ / 1000),
               static_cast<unsigned>(this->stage_worst_us_ % 1000));
      this->stage_started_ms_ = now;
      this->stage_slices_ = 0;
      this->stage_worst_us_ = 0;
      if (after != Stage::Ready && after != Stage::Refused) {
        ESP_LOGI(TAG, "boot: %s started (%d units)",
                 hearthmap_device::PanelPipeline::stage_name(after),
                 static_cast<int>(this->pipeline_->stage_total()));
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
  // A second buffer: frames render into the back one and swap it in whole (PanelPipeline).
  this->heat_back_ = allocator.allocate(raster.pixel_count());
  if (this->heat_back_ == nullptr) {
    // Nothing holds the first buffer yet (neither the pipeline nor the screen): give it back
    // rather than keep ~1.3 MiB for a component that has stopped.
    allocator.deallocate(this->heat_pixels_, raster.pixel_count());
    this->heat_pixels_ = nullptr;
    ESP_LOGE(TAG, "no memory for the second heat image buffer");
    this->phase_ = Phase::Failed;
    this->show_status_("hearthmap: not enough memory for the heat image");
    this->status_set_error(LOG_STR("no memory for the heat image"));
    return false;
  }
  std::fill_n(this->heat_back_, raster.pixel_count(), hm::Rgb565{0});
  this->pipeline_->set_heat_buffers(this->heat_pixels_, this->heat_back_);
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
  ESP_LOGI(TAG, "boot: screen done in %u ms (heat image %dx%d, 2 x %u bytes; rasterizer %u bytes)",
           static_cast<unsigned>(millis() - start), static_cast<int>(raster.width()),
           static_cast<int>(raster.height()), static_cast<unsigned>(raster.pixel_count() * 2U),
           static_cast<unsigned>(raster.working_bytes()));
  return true;
}

void HearthmapComponent::invalidate_band_() {
  // Repaint the newly swapped-in heat image a band at a time (kHeatBandRows), paced so LVGL
  // gets a refresh between bands. Every band shows the new image, consistent with the view
  // model already applied; bands not yet repainted keep the previous pixels on the glass until
  // their turn (well under a second).
  lv_obj_t* image = this->screen_->widgets().heat_image;
  const int32_t height = this->pipeline_->rasterizer().height();
  const uint32_t now = millis();
  if (image == nullptr || this->band_row_ >= height || now - this->last_band_ms_ < kBandIntervalMs)
    return;
  lv_area_t coords;
  lv_obj_get_coords(image, &coords);
  lv_area_t band = coords;
  band.y1 = coords.y1 + this->band_row_;
  band.y2 = std::min<int32_t>(coords.y1 + this->band_row_ + kHeatBandRows - 1, coords.y2);
  lv_obj_invalidate_area(image, &band);
  this->band_row_ += kHeatBandRows;
  this->last_band_ms_ = now;
}

void HearthmapComponent::refresh_view_(uint32_t start_ms, bool connected) {
  const hm::Timestamp now = this->clock_->timestamp_now();
  // R8: confirmed while the HA connection that subscribed to states is up; aging otherwise.
  hearthmap_device::restamp(this->readings_, connected, now);
  this->pipeline_->update(this->readings_, now, this->ui_state_, start_ms);
  // The top bar's summary from the same readings and time as the view model.
  this->summary_ = hm::host::house_summary(this->pipeline_->config(), this->readings_, now);
  this->connected_ = connected;
  this->dirty_ = false;
  this->last_view_ms_ = start_ms;
  if (!this->viewed_) ESP_LOGI(TAG, "first view model built");
  this->viewed_ = true;
}

void HearthmapComponent::show_view_() {
  hm::ui::HeaderText header;
  header.clock = this->clock_->now().strftime("%H:%M");
  header.summary = this->summary_;
  hm::ui::SourceStatus status;
  status.offline = !this->connected_;
  status.restamped = this->connected_;  // restamp() runs exactly while connected (D-046)
  status.heat_behind = this->pipeline_->heat_behind();
  this->dimmed_ = status.heat_behind;
  this->screen_->update(this->pipeline_->view(), header, this->clock_->timestamp_now(), status);
}

void HearthmapComponent::frame_slice_() {
  this->invalidate_band_();
  const uint32_t start = millis();
  // The view model: taps, a change of connection state, new readings and the clock reach the
  // screen within this slice, whatever the heat image is doing (D-047).
  const bool touched = !this->pending_.empty();
  for (const hm::UiEvent& e : this->pending_) {
    this->ui_state_ = hm::apply(this->pipeline_->config(), this->ui_state_, e);
  }
  this->pending_.clear();
  const bool connected = api::global_api_server->is_connected_with_state_subscription();
  const uint32_t since = start - this->last_view_ms_;
  const bool due = !this->viewed_ || touched || connected != this->connected_ ||
                   since >= this->refresh_interval_ms_ ||
                   (this->dirty_ && since >= this->min_redraw_interval_ms_);
  if (due) {
    this->refresh_view_(start, connected);
    this->show_view_();
  }

  // The heat image, in the rest of the slice: rebuilt only when the pipeline says so.
  bool idle = false;
  do {
    idle = this->pipeline_->heat_step(kFieldRowsPerStep, millis());
    if (!idle && !this->heat_active_) {
      this->heat_active_ = true;
      this->heat_started_ms_ = millis();
    }
  } while (!idle && millis() - start < kSliceBudgetMs);
  if (!idle) return;
  this->heat_active_ = false;
  if (!this->pipeline_->take_published()) return;

  // The finished back buffer is now the front: point the screen's image at it. If the map was
  // dimmed (a layer change, or the first image), undimming repaints it whole; otherwise it is
  // repainted in bands (invalidate_band_).
  const bool was_dimmed = this->dimmed_;
  this->heat_dsc_.data = reinterpret_cast<const uint8_t*>(this->pipeline_->front());
  if (was_dimmed) {
    this->band_row_ = 1 << 30;
  } else {
    this->band_row_ = 0;
    this->last_band_ms_ = 0;
  }
  this->show_view_();
  const uint32_t took = millis() - this->heat_started_ms_;
  if (!this->first_heat_logged_) {
    ESP_LOGI(TAG, "first heat image done in %u ms", static_cast<unsigned>(took));
    this->first_heat_logged_ = true;
  } else {
    ESP_LOGD(TAG, "heat image done in %u ms (%u rebuilds); LVGL warnings %u, errors %u so far",
             static_cast<unsigned>(took), static_cast<unsigned>(this->pipeline_->heat_rebuilds()),
             static_cast<unsigned>(lvgl_warnings()), static_cast<unsigned>(lvgl_errors()));
  }
}

void HearthmapComponent::show_status_(const char* text) {
  if (this->status_ == nullptr) {
    // The top layer of the display this component's LVGL instance drives (not LVGL's default
    // display, which can be another one when a device has two LVGL instances).
    this->status_ = lv_label_create(
        lv_display_get_layer_top(lv_obj_get_display(this->lvgl_->get_screen_active())));
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
                "  Photo images (all sizes): %u\n"
                "  Refresh interval: %u ms\n"
                "  Min redraw interval: %u ms\n"
                "  Heat refresh interval: %u ms",
                hearthmap_gen::kConfigSourceName,
                static_cast<unsigned>(hearthmap_gen::kSubscribedEntityCount),
                hearthmap_gen::kPhotoCount, static_cast<unsigned>(this->refresh_interval_ms_),
                static_cast<unsigned>(this->min_redraw_interval_ms_),
                static_cast<unsigned>(this->heat_refresh_interval_ms_));
  if (this->phase_ == Phase::Failed)
    ESP_LOGCONFIG(TAG, "  FAILED: config refused or out of memory, see errors above");
}

}  // namespace esphome::hearthmap_panel
