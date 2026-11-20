// Internal: small owned LVGL images (the placeholder leaf) and descriptor filling.
#pragma once

#include <cstdint>
#include <vector>

#include "lvgl.h"

namespace hearthmap::ui::detail {

/// Fill an image descriptor for a tightly packed buffer of `w` x `h` pixels in `cf`.
void fill_image_dsc(lv_image_dsc_t& dsc, lv_color_format_t cf, std::uint32_t w, std::uint32_t h,
                    std::uint32_t bytes_per_pixel, const void* data);

/// The placeholder plant photo: a leaf (a lens of two circles, tilted 45 degrees, with a darker
/// midrib) in `colour` on transparency, `size` x `size` pixels, ARGB8888. Antialiased by 4x4
/// supersampling in plain float arithmetic, so it is the same on every platform.
class LeafImage {
 public:
  LeafImage(std::int32_t size, std::uint32_t colour);
  LeafImage(const LeafImage&) = delete;
  LeafImage& operator=(const LeafImage&) = delete;
  const lv_image_dsc_t* dsc() const { return &dsc_; }

 private:
  std::vector<std::uint8_t> pixels_;
  lv_image_dsc_t dsc_{};
};

}  // namespace hearthmap::ui::detail
