#include "headless.h"

namespace hearthmap::host {

namespace {

std::uint32_t g_tick_ms = 0;
lv_point_t g_point{0, 0};
bool g_pressed = false;

std::uint32_t tick_cb() {
  return g_tick_ms;
}

void flush_cb(lv_display_t* disp, const lv_area_t* /*area*/, std::uint8_t* /*px_map*/) {
  // Direct mode: LVGL already drew into our framebuffer.
  lv_display_flush_ready(disp);
}

void read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  data->point = g_point;
  data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

HeadlessDisplay::HeadlessDisplay(std::int32_t width, std::int32_t height)
    : width_(width),
      height_(height),
      framebuffer_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0) {
  if (!lv_is_initialized()) lv_init();
  g_tick_ms = 0;
  g_pressed = false;
  lv_tick_set_cb(tick_cb);
  display_ = lv_display_create(width, height);
  lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display_, framebuffer_.data(), nullptr,
                         static_cast<std::uint32_t>(draw_buffer_bytes()),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display_, flush_cb);
  lv_display_set_default(display_);

  pointer_ = lv_indev_create();
  lv_indev_set_type(pointer_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer_, read_cb);
  lv_indev_set_display(pointer_, display_);
  // Reads happen only when tap() asks, so a timer run never sees a stray press.
  lv_timer_pause(lv_indev_get_read_timer(pointer_));
}

HeadlessDisplay::~HeadlessDisplay() {
  if (pointer_ != nullptr) lv_indev_delete(pointer_);
  if (display_ != nullptr) lv_display_delete(display_);
}

lv_obj_t* HeadlessDisplay::screen() const {
  return lv_display_get_screen_active(display_);
}

void HeadlessDisplay::set_time_ms(std::uint32_t ms) {
  g_tick_ms = ms;
}

std::uint32_t HeadlessDisplay::time_ms() const {
  return g_tick_ms;
}

void HeadlessDisplay::run_timers() {
  lv_timer_handler();
}

void HeadlessDisplay::tap(std::int32_t x, std::int32_t y) {
  lv_obj_update_layout(screen());
  g_point.x = x;
  g_point.y = y;
  g_pressed = true;
  lv_indev_read(pointer_);
  g_pressed = false;
  lv_indev_read(pointer_);
}

void HeadlessDisplay::render() {
  lv_obj_update_layout(screen());
  lv_obj_invalidate(screen());
  lv_refr_now(display_);
}

}  // namespace hearthmap::host
