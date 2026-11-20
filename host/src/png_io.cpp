#include "png_io.h"

#include <lodepng.h>

namespace hearthmap::host {

RgbImage from_rgb565(const std::vector<std::uint16_t>& pixels, std::uint32_t width,
                     std::uint32_t height) {
  RgbImage img;
  img.width = width;
  img.height = height;
  img.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
  for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height && i < pixels.size(); ++i) {
    const unsigned c = pixels[i];
    const unsigned r5 = (c >> 11U) & 0x1FU;
    const unsigned g6 = (c >> 5U) & 0x3FU;
    const unsigned b5 = c & 0x1FU;
    img.rgb[i * 3 + 0] = static_cast<std::uint8_t>((r5 << 3U) | (r5 >> 2U));
    img.rgb[i * 3 + 1] = static_cast<std::uint8_t>((g6 << 2U) | (g6 >> 4U));
    img.rgb[i * 3 + 2] = static_cast<std::uint8_t>((b5 << 3U) | (b5 >> 2U));
  }
  return img;
}

std::string write_png(const std::string& path, const RgbImage& image) {
  const unsigned err = lodepng::encode(path, image.rgb, image.width, image.height, LCT_RGB, 8);
  return err == 0 ? std::string() : std::string(lodepng_error_text(err));
}

std::string read_png(const std::string& path, RgbImage& out) {
  unsigned w = 0;
  unsigned h = 0;
  out.rgb.clear();
  const unsigned err = lodepng::decode(out.rgb, w, h, path, LCT_RGB, 8);
  if (err != 0) return std::string(lodepng_error_text(err));
  out.width = w;
  out.height = h;
  return std::string();
}

std::string read_png_rgba(const std::string& path, std::vector<std::uint8_t>& rgba,
                          std::uint32_t& width, std::uint32_t& height) {
  unsigned w = 0;
  unsigned h = 0;
  rgba.clear();
  const unsigned err = lodepng::decode(rgba, w, h, path, LCT_RGBA, 8);
  if (err != 0) return std::string(lodepng_error_text(err));
  width = w;
  height = h;
  return std::string();
}

}  // namespace hearthmap::host
