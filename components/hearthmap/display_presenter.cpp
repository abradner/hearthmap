#include "display_presenter.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_rotation.h"

namespace esphome::hearthmap_panel {

namespace ui = ::hearthmap::ui;

DisplayPresenter::~DisplayPresenter() {
  heap_caps_free(buf_);
}

bool DisplayPresenter::begin() {
  task_ = xTaskGetCurrentTaskHandle();
  buf_ = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, kMaxTilePx * sizeof(uint16_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return buf_ != nullptr;
}

bool DisplayPresenter::present(int x, int y, const ui::View565& tile) {
  const int w = tile.w, h = tile.h;
  const int nw = display_->get_native_width(), nh = display_->get_native_height();
  const bool quarter = rotation_ == display::DISPLAY_ROTATION_90_DEGREES ||
                       rotation_ == display::DISPLAY_ROTATION_270_DEGREES;
  const int sw = quarter ? nh : nw, sh = quarter ? nw : nh;  // screen (LVGL) size
  if (buf_ == nullptr || xTaskGetCurrentTaskHandle() != task_ || tile.px == nullptr || w <= 0 ||
      h <= 0 || w * h > kMaxTilePx || x < 0 || y < 0 || x + w > sw || y + h > sh ||
      rotation_ == display::DISPLAY_ROTATION_180_DEGREES) {
    refused_++;
    return false;
  }
  const int turns = rotation_ == display::DISPLAY_ROTATION_90_DEGREES    ? 1
                    : rotation_ == display::DISPLAY_ROTATION_270_DEGREES ? 3
                                                                         : 0;
  const NativeBlock b = to_native(buf_, tile.px, tile.stride, x, y, w, h, turns, nw, nh);
  display_->draw_pixels_at(b.x, b.y, b.w, b.h, reinterpret_cast<const uint8_t*>(buf_),
                           display::COLOR_BITNESS_565, false);
  presented_++;
  return true;
}

}  // namespace esphome::hearthmap_panel
