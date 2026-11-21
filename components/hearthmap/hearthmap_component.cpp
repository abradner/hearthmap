#include "hearthmap_component.h"

#include "stall_monitor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "esp_heap_caps.h"
#include "esphome/components/api/api_server.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/format.h"
#include "hearthmap/ui/theme.h"
#include "hearthmap_gen/photos.h"
#include "readings_ingest.h"

namespace esphome::hearthmap_panel {

static const char* const TAG = "hearthmap";

namespace {

// Work per loop() call: steps run until this much time has gone, then loop() returns so the
// watchdog, Wi-Fi, the API and LVGL get the CPU. One step is a few rows (core's RoomScanner,
// the rasterizer's build, heat-field rows), small enough that a slice overruns the budget by at
// most one step even at the schema's maximums (components/hearthmap/README.md, "Cost").
constexpr uint32_t kSliceBudgetMs = 20;
// Heat work once the screen is running: shorter, so a pulse frame (one every 80 ms per ring,
// rings offset in phase, so one due every few milliseconds) waits at most this long behind it
// (D-056). A rebuild takes longer in wall time; nothing waits on it.
constexpr uint32_t kHeatSliceBudgetMs = 8;
// After a new heat image is swapped in, the map is invalidated in bands of this many rows, one
// band per loop() call at most every kBandIntervalMs, so LVGL repaints it over several refreshes
// instead of in one long one (a full 920 x 736 repaint took 645 ms on the panel).
constexpr int32_t kHeatBandRows = 48;
constexpr uint32_t kBandIntervalMs = 25;
// After a press, heat work waits this long, so the slices of a rebuild never sit between a
// touch and the refresh that answers it (the tap, its click and the screen update all happen
// within a few loop passes).
constexpr uint32_t kHeatHoldAfterPressMs = 300;
constexpr std::int32_t kBootRowsPerStep = 4;
// PSRAM left free when allocating extra heat-image buffers (for LVGL, sprites, the bake, Wi-Fi).
constexpr std::size_t kPsramReserve = 6U * 1024U * 1024U;
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

static const char* present_block_name(hm::ui::PresentBlock b) {
  using B = hm::ui::PresentBlock;
  switch (b) {
    case B::Ok: return "ok (presenter refused)";
    case B::NotImage: return "not-image";
    case B::NotRgb565: return "not-rgb565";
    case B::Refreshing: return "refreshing";
    case B::NotOnActiveScreen: return "not-active-screen";
    case B::Hidden: return "hidden";
    case B::NotOpaque: return "not-opaque";
    case B::Transformed: return "transformed";
    case B::SizeMismatch: return "size";
    case B::Clipped: return "clipped";
    case B::HasChildren: return "children";
    case B::Covered: return "covered";
  }
  return "?";
}

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
  this->blitter_ = make_blitter(this->blitter_kind_, &this->blitter_name_);
  // Names the main loop's wait if it ever stops turning (D-062).
  breadcrumb().turn_ms.store(millis(), std::memory_order_relaxed);
  this->stall_monitor_ = start_stall_monitor(this->watchdog_ms_);
  this->presenter_ = std::make_unique<hm::ui::NoPresenter>();
  if (this->present_display_ != nullptr && this->lvgl_ != nullptr) {
    auto direct =
        std::make_unique<DisplayPresenter>(this->present_display_, this->lvgl_->get_rotation());
    if (direct->begin()) {
      this->presenter_ = std::move(direct);
      this->presenter_name_ = "display";
      this->presenting_ = true;
    } else {
      this->presenter_name_ = "off (no internal RAM for the rotation buffer)";
    }
  }
  hearthmap_device::PanelPipeline::Builder builder;
  builder.steps = this->source_.build_steps();
  builder.step = [this](hm::Config& c, std::size_t k) { this->source_.build_step(c, k); };
  this->pipeline_ = std::make_unique<hearthmap_device::PanelPipeline>(std::move(builder),
                                                                      hm::ui::theme::kHeatPalette);
  this->pipeline_->set_heat_refresh_interval_ms(this->heat_refresh_interval_ms_);
  hook_lvgl_log();  // after the lvgl component's setup, whose callback this takes over
  this->hook_perf_();
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
  // The stall monitor's view (D-062): the loop turned, and is in hearthmap until this returns.
  breadcrumb().turn_ms.store(millis(), std::memory_order_relaxed);
  const CrumbScope crumb(Crumb::Hearthmap, millis());
  this->perf_.loop_start(micros());
  this->check_sprite_canaries_();
  this->log_perf_();
  switch (this->phase_) {
    case Phase::Booting: {
      const uint32_t t0 = micros();
      this->boot_slice_();
      this->perf_.own_work(0, micros() - t0);  // boot work is counted with the heat work
      return;
    }
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
  // Every buffer starts black, cleared by the blitter (a PPA fill on the panel, not 1.3 MiB of
  // CPU stores per buffer).
  auto clear = [this, &raster](hm::Rgb565* px) {
    const hm::ui::Surface565 s{px, static_cast<int>(raster.width()),
                               static_cast<int>(raster.height()), static_cast<int>(raster.width())};
    this->blitter_->fill(s, 0, 0, s.w, s.h, 0);
  };
  this->heat_pixels_ = allocator.allocate(raster.pixel_count());
  if (this->heat_pixels_ == nullptr) {
    ESP_LOGE(TAG, "no memory for the %dx%d heat image", static_cast<int>(raster.width()),
             static_cast<int>(raster.height()));
    this->phase_ = Phase::Failed;
    this->show_status_("hearthmap: not enough memory for the heat image");
    this->status_set_error(LOG_STR("no memory for the heat image"));
    return false;
  }
  clear(this->heat_pixels_);
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
  clear(this->heat_back_);
  // More buffers, one per enabled layer plus one to draw into, while PSRAM stays comfortable:
  // then every layer keeps its image and a tab change shows it at once (D-055). Fewer is fine,
  // only slower on a tab change.
  std::vector<hm::Rgb565*> buffers{this->heat_pixels_, this->heat_back_};
  std::size_t enabled = 0;
  for (const auto& layer : this->pipeline_->config().layers) enabled += layer.enabled ? 1U : 0U;
  const std::size_t bytes = raster.pixel_count() * sizeof(hm::Rgb565);
  while (buffers.size() < enabled + 1 &&
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > bytes + kPsramReserve) {
    hm::Rgb565* extra = allocator.allocate(raster.pixel_count());
    if (extra == nullptr) break;
    clear(extra);
    buffers.push_back(extra);
  }
  this->pipeline_->set_heat_buffers(buffers);
  ESP_LOGI(TAG, "boot: %u heat image buffers for %u layers", static_cast<unsigned>(buffers.size()),
           static_cast<unsigned>(enabled));
  this->heat_dsc_ =
      rgb565_dsc(reinterpret_cast<const uint8_t*>(this->heat_pixels_),
                 static_cast<uint32_t>(raster.width()), static_cast<uint32_t>(raster.height()));
  this->photos_.init();
  hm::ui::ScreenDeps deps;
  deps.fonts = hm::ui::builtin_fonts();
  deps.sink = this;
  deps.photos = &this->photos_;
  deps.micros = [] { return static_cast<std::uint32_t>(micros()); };
  // Pulse sprites hold every frame of every pulse (D-053): three quarters of the PSRAM left once
  // kPsramReserve and the map bake (3 bytes a map pixel) are set aside, at least 256 KiB.
  // Past it they take fewer frames per cycle, then fewer pulses animate (D-054).
  const std::size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const std::size_t bake = raster.pixel_count() * 3U;
  const std::size_t spare =
      free_psram > kPsramReserve + bake ? free_psram - kPsramReserve - bake : 0U;
  deps.pulse_memory_bytes = std::max<std::size_t>(256U * 1024U, spare / 4U * 3U);
  ESP_LOGI(TAG, "boot: %u KiB for pulse sprites",
           static_cast<unsigned>(deps.pulse_memory_bytes / 1024U));
  this->screen_ = std::make_unique<hm::ui::Screen>(
      this->lvgl_->get_screen_active(), this->pipeline_->config(), &this->heat_dsc_, deps);
  // With a presenter, pulse frames are shown from loop(), straight to the panel where they can be
  // (D-058); otherwise LVGL's timer invalidates them as before.
  this->screen_->set_frames_deferred(this->presenting_);
  // Rows of each new heat image get the walls and labels once the bake is ready (D-055).
  this->pipeline_->set_row_hook(
      [this](hm::Rgb565* image, std::int32_t stride, std::int32_t begin, std::int32_t end) {
        return this->screen_ != nullptr && this->screen_->bake_rows(image, stride, begin, end);
      });
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
  this->perf_.cause_heat();
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
  this->connected_ = connected;
  this->dirty_ = false;
  this->last_view_ms_ = start_ms;
  if (!this->viewed_) ESP_LOGI(TAG, "first view model built");
  this->viewed_ = true;
}

void HearthmapComponent::show_view_() {
  hm::ui::HeaderText header;
  header.clock = this->clock_->now().strftime("%H:%M");
  header.summary = hm::ui::summary_text(this->pipeline_->view().summary);
  hm::ui::SourceStatus status;
  status.offline = !this->connected_;
  status.restamped = this->connected_;  // restamp() runs exactly while connected (D-046)
  status.heat_behind = this->pipeline_->heat_behind();
  status.heat_baked = this->pipeline_->front_baked();
  this->dimmed_ = status.heat_behind;
  this->screen_->update(this->pipeline_->view(), header, this->clock_->timestamp_now(), status);
}

void HearthmapComponent::frame_slice_() {
  // Pulse frames LVGL's timer chose since the last pass: presented, or invalidated (D-058).
  this->screen_->show_due_frames(*this->presenter_, &this->shown_frames_);
  const uint32_t view_t0 = micros();
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
  const std::size_t writes_before = this->screen_->writes();
  if (due) {
    this->refresh_view_(start, connected);
    this->show_view_();
    this->handle_publish_();  // a tab change to a kept image is shown in this same call
  }
  if (touched) {
    const bool changed = this->screen_->writes() != writes_before;
    this->perf_.events_applied(millis(), changed);
    if (changed) this->perf_.cause_tap();  // a tap that changed nothing draws nothing
  }
  const uint32_t heat_t0 = micros();
  // Counts this call's work when it returns, whichever way it returns.
  struct OwnTimer {
    hearthmap_device::PerfStats& perf;
    uint32_t view_us;
    uint32_t heat_t0;
    ~OwnTimer() { perf.own_work(view_us, micros() - heat_t0); }
  } own_timer{this->perf_, heat_t0 - view_t0, heat_t0};

  // The heat image, in the rest of the slice: rebuilt only when the pipeline says so, and not
  // just after a press.
  if (this->pressed_ && millis() - this->last_press_ms_ < kHeatHoldAfterPressMs) return;
  this->pressed_ = false;
  // The map bake and the pulse sprites first (one band or tile per step, a small software render
  // each), before the next heat work (D-053, D-055). When the bake becomes ready every kept
  // image is redrawn with the walls and labels in it.
  // One unit per slice, then heat work in what is left: a queue that keeps refilling (a value
  // near a pulse changing every pass) must not hold the heat image back.
  this->screen_->prepare_step();
  if (!this->rebake_requested_ && this->screen_->bake_ready()) {
    this->rebake_requested_ = true;
    this->pipeline_->request_rebuild_all();
  }
  bool idle = false;
  do {
    idle = this->pipeline_->heat_step(kFieldRowsPerStep, millis());
    if (!idle && !this->heat_active_) {
      this->heat_active_ = true;
      this->heat_started_ms_ = millis();
    }
    // Until the first image is up there is nothing to animate: the boot budget applies.
  } while (!idle &&
           millis() - start < (this->first_heat_logged_ ? kHeatSliceBudgetMs : kSliceBudgetMs));
  if (!idle) return;
  this->heat_active_ = false;
  this->handle_publish_();
}

void HearthmapComponent::handle_publish_() {
  if (!this->pipeline_->take_published()) return;
  // The new front image: point the screen's image at it. A layer change to a kept image is
  // repainted whole at once (D-055: one opaque blit of the map); an image that was hidden as
  // behind (the first, or a layer never kept) is revealed by the Screen a strip at a time
  // (D-052); a fresh image of the shown layer is repainted in bands here (invalidate_band_).
  const bool was_dimmed = this->dimmed_;
  this->heat_dsc_.data = reinterpret_cast<const uint8_t*>(this->pipeline_->front());
  if (this->pipeline_->published_switch()) {
    this->band_row_ = 1 << 30;
    this->screen_->invalidate_heat();
  } else if (was_dimmed) {
    this->band_row_ = 1 << 30;
  } else {
    this->band_row_ = 0;
    this->last_band_ms_ = 0;
  }
  this->show_view_();
  if (this->pipeline_->published_switch()) return;  // no new image was drawn
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

void HearthmapComponent::hook_perf_() {
  // LVGL's own events for the display this component draws on: every refresh, every flush and
  // every invalidation. The lvgl component registered its rounder before this, so the areas
  // counted here are the rounded ones it actually renders.
  lv_display_t* disp = lv_obj_get_display(this->lvgl_->get_screen_active());
  for (lv_event_code_t code : {LV_EVENT_INVALIDATE_AREA, LV_EVENT_REFR_START, LV_EVENT_REFR_READY,
                               LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH}) {
    lv_display_add_event_cb(disp, on_display_event_, code, this);
  }
  // Presses and clicks, from every pointer input device on that display.
  for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
       indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER || lv_indev_get_display(indev) != disp)
      continue;
    lv_indev_add_event_cb(indev, on_indev_event_, LV_EVENT_PRESSED, this);
    lv_indev_add_event_cb(indev, on_indev_event_, LV_EVENT_CLICKED, this);
  }
#ifdef USE_TOUCHSCREEN
  if (this->touchscreen_ != nullptr) {
    this->touch_probe_ = std::make_unique<TouchProbe>(&this->perf_);
    this->touchscreen_->register_listener(this->touch_probe_.get());
  }
#endif
}

void HearthmapComponent::on_display_event_(lv_event_t* e) {
  auto* self = static_cast<HearthmapComponent*>(lv_event_get_user_data(e));
  const uint32_t now_us = micros();
  switch (lv_event_get_code(e)) {
    case LV_EVENT_INVALIDATE_AREA: {
      const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(e));
      if (area != nullptr) self->perf_.invalidated(lv_area_get_size(area));
      break;
    }
    case LV_EVENT_REFR_START:
      self->crumb_before_refresh_ = mark_crumb(Crumb::LvglRefresh, millis());
      self->perf_.refresh_start(now_us);
      break;
    case LV_EVENT_REFR_READY:
      mark_crumb(self->crumb_before_refresh_, millis());
      self->perf_.refresh_ready(now_us, millis());
      break;
    case LV_EVENT_FLUSH_START:
      mark_crumb(Crumb::LvglFlush, millis());
      self->perf_.flush_start(now_us);
      break;
    case LV_EVENT_FLUSH_FINISH: {
      mark_crumb(Crumb::LvglRefresh, millis());
      const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(e));
      self->perf_.flush_finish(now_us, area != nullptr ? lv_area_get_size(area) : 0);
      break;
    }
    default: break;
  }
}

void HearthmapComponent::on_indev_event_(lv_event_t* e) {
  auto* self = static_cast<HearthmapComponent*>(lv_event_get_user_data(e));
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
    self->perf_.lvgl_pressed(millis());
    self->pressed_ = true;
    self->last_press_ms_ = millis();
  }
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) self->perf_.lvgl_clicked(millis());
}

#ifdef USE_TOUCHSCREEN
void TouchProbe::touch(touchscreen::TouchPoint /*tp*/) {
  this->stats_->touch_read(millis());
}
#endif

// Once a second: every pulse frame set's guard words, so a write outside a set is reported where
// it happened rather than as a corrupt picture (D-053). A few words per set.
void HearthmapComponent::check_sprite_canaries_() {
  const uint32_t now = millis();
  if (!this->screen_ || now - this->last_canary_check_ms_ < 1000U) return;
  this->last_canary_check_ms_ = now;
  const uint32_t bad = this->screen_->pulse_canary_failures();
  if (bad == 0) return;
  this->canary_failures_ += bad;
  if (!this->canary_logged_) {
    this->canary_logged_ = true;
    ESP_LOGE(TAG, "pulse sprites: %u frame set(s) written outside their bounds",
             static_cast<unsigned>(bad));
  }
}

void HearthmapComponent::log_perf_() {
  const uint32_t now = millis();
  if (!this->perf_.due(now)) return;
  hearthmap_device::PerfWindow w = this->perf_.take(now);
  if (this->screen_) {
    const hm::ui::PulseCounters c = this->screen_->pulse_counters();
    w.sprite_frames = c.frames - this->last_sprite_frames_;
    w.sprite_dropped = c.dropped - this->last_sprite_dropped_;
    w.sprite_kib = static_cast<uint32_t>(c.tile_bytes / 1024U);
    w.sprite_divisor = c.frame_divisor;
    w.sprite_canary = this->canary_failures_;
    w.sprite_not_ready = c.not_ready - this->last_sprite_not_ready_;
    this->last_sprite_not_ready_ = c.not_ready;
    w.sprite_frame_us = static_cast<uint32_t>(c.frame_us - this->last_sprite_frame_us_);
    w.sprite_prepare_us = static_cast<uint32_t>(c.prepare_us - this->last_sprite_prepare_us_);
    this->last_sprite_frame_us_ = c.frame_us;
    w.presented = this->shown_frames_.presented;
    w.present_fallbacks = this->shown_frames_.invalidated;
    std::size_t top = 0;
    for (std::size_t k = 1; k < this->shown_frames_.blocked.size(); ++k) {
      if (this->shown_frames_.blocked[k] > this->shown_frames_.blocked[top]) top = k;
    }
    w.present_block = this->shown_frames_.blocked[top] == 0
                          ? "none"
                          : present_block_name(static_cast<hm::ui::PresentBlock>(top));
    this->shown_frames_ = hm::ui::Screen::ShownFrames{};
    this->last_sprite_prepare_us_ = c.prepare_us;
    this->last_sprite_frames_ = c.frames;
    this->last_sprite_dropped_ = c.dropped;
  }
  char line[640];
  hearthmap_device::PerfStats::format(w, line, sizeof(line));
  ESP_LOGD(TAG, "%s", line);
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
  ESP_LOGCONFIG(TAG, "hearthmap blitter: %s, present: %s", this->blitter_name_,
                this->presenter_name_);
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
