#include "image_util.h"

namespace hearthmap::ui::detail {

#if defined(__GNUC__)
#pragma GCC diagnostic push
// lv_image_header_t is a set of bit-fields; every value written here is in range.
#pragma GCC diagnostic ignored "-Wconversion"
#endif
void fill_image_dsc(lv_image_dsc_t& dsc, lv_color_format_t cf, std::uint32_t w, std::uint32_t h,
                    std::uint32_t bytes_per_pixel, const void* data) {
  dsc = lv_image_dsc_t{};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = cf;
  dsc.header.w = w;
  dsc.header.h = h;
  dsc.header.stride = w * bytes_per_pixel;
  dsc.data_size = w * h * bytes_per_pixel;
  dsc.data = static_cast<const std::uint8_t*>(data);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

// Coverage of the leaf at a point in unit coordinates (-1..1 on both axes): 0 outside,
// 1 leaf, 2 midrib.
int leaf_sample(float x, float y) {
  // Rotate by 45 degrees so the leaf points to the top-right.
  constexpr float k = 0.70710678F;
  const float u = (x - y) * k;  // along the leaf
  const float v = (x + y) * k;  // across the leaf
  constexpr float r = 1.0F;
  constexpr float d = 0.62F;
  const float a = (v - d) * (v - d) + u * u;
  const float b = (v + d) * (v + d) + u * u;
  if (a > r * r || b > r * r) return 0;
  if (v > -0.045F && v < 0.045F && u > -0.70F && u < 0.62F) return 2;
  return 1;
}

}  // namespace

LeafImage::LeafImage(std::int32_t size, std::uint32_t colour) {
  const auto n = static_cast<std::uint32_t>(size > 0 ? size : 1);
  pixels_.assign(static_cast<std::size_t>(n) * n * 4U, 0);
  const auto cr = static_cast<std::uint8_t>((colour >> 16U) & 0xFFU);
  const auto cg = static_cast<std::uint8_t>((colour >> 8U) & 0xFFU);
  const auto cb = static_cast<std::uint8_t>(colour & 0xFFU);
  // The midrib is the leaf colour at 60%.
  const auto dim = [](std::uint8_t c) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(c) * 3U) / 5U);
  };
  constexpr int kSs = 4;
  const float scale = 2.0F / static_cast<float>(n);
  for (std::uint32_t py = 0; py < n; ++py) {
    for (std::uint32_t px = 0; px < n; ++px) {
      int leaf = 0;
      int rib = 0;
      for (int sy = 0; sy < kSs; ++sy) {
        for (int sx = 0; sx < kSs; ++sx) {
          const float x =
              (static_cast<float>(px) + (static_cast<float>(sx) + 0.5F) / kSs) * scale - 1.0F;
          const float y =
              (static_cast<float>(py) + (static_cast<float>(sy) + 0.5F) / kSs) * scale - 1.0F;
          const int s = leaf_sample(x, -y);
          if (s >= 1) ++leaf;
          if (s == 2) ++rib;
        }
      }
      if (leaf == 0) continue;
      const std::size_t i = (static_cast<std::size_t>(py) * n + px) * 4U;
      const bool is_rib = rib * 2 >= leaf;
      pixels_[i + 0] = is_rib ? dim(cb) : cb;
      pixels_[i + 1] = is_rib ? dim(cg) : cg;
      pixels_[i + 2] = is_rib ? dim(cr) : cr;
      pixels_[i + 3] = static_cast<std::uint8_t>((leaf * 255 + (kSs * kSs) / 2) / (kSs * kSs));
    }
  }
  fill_image_dsc(dsc_, LV_COLOR_FORMAT_ARGB8888, n, n, 4U, pixels_.data());
}

}  // namespace hearthmap::ui::detail
