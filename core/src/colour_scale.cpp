// colour_scale.cpp — the comfort-quiet diverging scale and its lookup table (colour_scale.h).
#include "hearthmap/colour_scale.h"

#include <cmath>
#include <cstddef>

namespace hearthmap {

Rgb888 colour_for(const Band& band, double value, const Palette& palette) {
  if (!std::isfinite(value)) return palette.neutral;
  if (value < band.comfort_min) {
    const double span = band.comfort_min - band.scale_min;
    if (!(span > 0.0)) return palette.neutral;  // no gradient room: stay quiet (R9)
    return lerp(palette.neutral, palette.cold, (band.comfort_min - value) / span);
  }
  if (value > band.comfort_max) {
    const double span = band.scale_max - band.comfort_max;
    if (!(span > 0.0)) return palette.neutral;  // no gradient room: stay quiet (R9)
    return lerp(palette.neutral, palette.hot, (value - band.comfort_max) / span);
  }
  return palette.neutral;  // comfort_min <= value <= comfort_max, edges included
}

ColourTable make_colour_table(const Band& band, const Palette& palette) {
  ColourTable table;
  constexpr auto kLast = static_cast<double>(ColourTable::kEntries - 1);
  for (std::size_t i = 0; i < ColourTable::kEntries; ++i) {
    const double v =
        band.scale_min + static_cast<double>(i) * (band.scale_max - band.scale_min) / kLast;
    table.entries[i] = colour565_for(band, v, palette);
  }
  table.neutral = to_rgb565(palette.neutral);
  table.comfort_min = static_cast<float>(band.comfort_min);
  table.comfort_max = static_cast<float>(band.comfort_max);
  table.scale_min = static_cast<float>(band.scale_min);
  table.scale_max = static_cast<float>(band.scale_max);
  return table;
}

Rgb565 ColourTable::lookup(float value) const {
  if (!std::isfinite(value)) return neutral;
  if (value >= comfort_min && value <= comfort_max) return neutral;
  if (value <= scale_min) return entries.front();
  if (value >= scale_max) return entries.back();
  // scale_min < value < scale_max here, so the position is in (0, kEntries - 1).
  constexpr auto kLast = static_cast<float>(kEntries - 1);
  const float position = (value - scale_min) / (scale_max - scale_min) * kLast;
  const auto index = static_cast<std::size_t>(position + 0.5F);
  return entries[index < kEntries ? index : kEntries - 1];
}

}  // namespace hearthmap
