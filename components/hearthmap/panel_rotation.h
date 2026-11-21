// panel_rotation.h — undoing LVGL's software rotation for one block, as the lvgl component's
// flush does (esphome/components/lvgl/lvgl_esphome.cpp, draw_buffer_). Pure; host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::hearthmap_panel {

/// Where a screen-space block lands on the native panel, and its native size.
struct NativeBlock {
  int x, y, w, h;
};

/// `quarter_turns`: 0, 1 (LVGL rotation 90) or 3 (270). Writes the w x h block `src` (stride in
/// pixels) into `dst` as the native block (tightly packed) and returns where it goes on a panel
/// `native_w` x `native_h`, for a block at screen (x, y). Other values of quarter_turns are not
/// handled: the result has w == 0.
inline NativeBlock to_native(std::uint16_t* dst, const std::uint16_t* src, int stride, int x, int y,
                             int w, int h, int quarter_turns, int native_w, int native_h) {
  if (quarter_turns == 1) {
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        dst[static_cast<std::ptrdiff_t>(c) * h + (h - 1 - r)] = src[r * stride + c];
    return {native_w - y - h, x, h, w};
  }
  if (quarter_turns == 3) {
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        dst[static_cast<std::ptrdiff_t>(w - 1 - c) * h + r] = src[r * stride + c];
    return {y, native_h - x - w, h, w};
  }
  if (quarter_turns == 0) {
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c) dst[r * w + c] = src[r * stride + c];
    return {x, y, w, h};
  }
  return {0, 0, 0, 0};
}

}  // namespace esphome::hearthmap_panel
