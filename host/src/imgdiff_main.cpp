// hearthmap-imgdiff: exact comparison of two PNGs.
//
//   hearthmap-imgdiff <expected.png> <actual.png> [<diff.png>]
//
// Exit 0 when every pixel is identical, 1 when any differs (or the sizes do), 2 on a usage or
// read error. On a difference it prints the count and the bounding box, and writes <diff.png>:
// the actual image dimmed to a quarter, with every differing pixel in solid magenta.
#include <algorithm>
#include <cstdio>
#include <string>

#include "png_io.h"

int main(int argc, char** argv) {
  using hearthmap::host::RgbImage;
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr, "usage: hearthmap-imgdiff <expected.png> <actual.png> [<diff.png>]\n");
    return 2;
  }
  RgbImage expected;
  RgbImage actual;
  std::string err = hearthmap::host::read_png(argv[1], expected);
  if (!err.empty()) {
    std::fprintf(stderr, "hearthmap-imgdiff: %s: %s\n", argv[1], err.c_str());
    return 2;
  }
  err = hearthmap::host::read_png(argv[2], actual);
  if (!err.empty()) {
    std::fprintf(stderr, "hearthmap-imgdiff: %s: %s\n", argv[2], err.c_str());
    return 2;
  }
  if (expected.width != actual.width || expected.height != actual.height) {
    std::fprintf(stderr, "hearthmap-imgdiff: size differs: expected %ux%u, actual %ux%u\n",
                 expected.width, expected.height, actual.width, actual.height);
    return 1;
  }
  std::size_t differing = 0;
  std::uint32_t x0 = actual.width, y0 = actual.height, x1 = 0, y1 = 0;
  RgbImage diff = actual;
  for (std::uint32_t y = 0; y < actual.height; ++y) {
    for (std::uint32_t x = 0; x < actual.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * actual.width + x) * 3U;
      const bool same = std::equal(expected.rgb.begin() + static_cast<std::ptrdiff_t>(i),
                                   expected.rgb.begin() + static_cast<std::ptrdiff_t>(i + 3),
                                   actual.rgb.begin() + static_cast<std::ptrdiff_t>(i));
      if (same) {
        for (int c = 0; c < 3; ++c) diff.rgb[i + static_cast<std::size_t>(c)] /= 4;
        continue;
      }
      ++differing;
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
      diff.rgb[i] = 0xFF;
      diff.rgb[i + 1] = 0x00;
      diff.rgb[i + 2] = 0xFF;
    }
  }
  if (differing == 0) return 0;
  std::fprintf(stderr, "hearthmap-imgdiff: %zu pixel(s) differ, within x %u..%u, y %u..%u\n",
               differing, x0, x1, y0, y1);
  if (argc == 4) {
    err = hearthmap::host::write_png(argv[3], diff);
    if (!err.empty()) std::fprintf(stderr, "hearthmap-imgdiff: %s: %s\n", argv[3], err.c_str());
  }
  return 1;
}
