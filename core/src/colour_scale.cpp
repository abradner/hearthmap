// colour_scale.cpp — the comfort-quiet diverging scale and its lookup table (colour_scale.h).
#include "hearthmap/colour_scale.h"

#include <cmath>
#include <cstddef>

namespace hearthmap {

Rgb888 colour_for(const Band& band, double value, const Palette& palette) {
  if (!std::isfinite(value)) return palette.neutral;
  if (value < band.comfort_min) {
    const double span = band.comfort_min - band.scale_min;
    if (!(span > 0.0)) return palette.neutral;  // no gradient room: stay quiet (D-021)
    return lerp(palette.neutral, palette.cold, (band.comfort_min - value) / span);
  }
  if (value > band.comfort_max) {
    const double span = band.scale_max - band.comfort_max;
    if (!(span > 0.0)) return palette.neutral;  // no gradient room: stay quiet (D-021)
    return lerp(palette.neutral, palette.hot, (value - band.comfort_max) / span);
  }
  return palette.neutral;  // comfort_min <= value <= comfort_max, edges included
}

void build_colour_lut(const Band& band, const Palette& palette, std::size_t size, ColourLut& out) {
  const std::size_t n = size < 2 ? 2 : size;
  out.lo = static_cast<float>(band.scale_min);
  out.hi = static_cast<float>(band.scale_max);
  out.entries.resize(n);  // reuses storage once it has held n entries
  const auto last = static_cast<double>(n - 1);
  for (std::size_t i = 0; i < n; ++i) {
    const double v =
        band.scale_min + (band.scale_max - band.scale_min) * static_cast<double>(i) / last;
    out.entries[i] = colour565_for(band, v, palette);
  }
}

ColourLut build_colour_lut(const Band& band, const Palette& palette, std::size_t size) {
  ColourLut lut;
  build_colour_lut(band, palette, size, lut);
  return lut;
}

Rgb565 ColourLut::lookup(float value) const {
  if (entries.empty() || !std::isfinite(value)) return 0;
  const auto last = static_cast<float>(entries.size() - 1);
  // A degenerate scale (hi <= lo, rejected by validate()) has no interior: saturate by side.
  if (!(hi > lo)) return value <= lo ? entries.front() : entries.back();
  float position = (value - lo) / (hi - lo) * last;
  if (!(position > 0.0F)) return entries.front();
  if (position > last) position = last;
  return entries[static_cast<std::size_t>(position + 0.5F)];
}

}  // namespace hearthmap
