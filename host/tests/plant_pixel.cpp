// plant_pixel <in.png> <out.png> <x> <y>: copy a PNG with one pixel's colour inverted.
// Used to prove the golden comparison can fail (comparison_can_fail.cmake).
#include <cstdio>
#include <cstdlib>

#include "png_io.h"

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: plant_pixel <in.png> <out.png> <x> <y>\n");
    return 2;
  }
  hearthmap::host::RgbImage img;
  if (!hearthmap::host::read_png(argv[1], img).empty()) return 2;
  const auto x = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
  const auto y = static_cast<std::size_t>(std::strtoul(argv[4], nullptr, 10));
  if (x >= img.width || y >= img.height) return 2;
  const std::size_t i = (y * img.width + x) * 3U;
  for (std::size_t c = 0; c < 3; ++c) img.rgb[i + c] = static_cast<std::uint8_t>(~img.rgb[i + c]);
  return hearthmap::host::write_png(argv[2], img).empty() ? 0 : 2;
}
