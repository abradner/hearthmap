// TEMPORARY: stand-ins for the core functions ui_lvgl calls, so its tests can link before
// core's implementations land. Each follows its header's contract exactly. Delete this file
// (and its line in CMakeLists.txt) when core provides them.
#include <cmath>

#include "hearthmap/colour_scale.h"
#include "hearthmap/geometry.h"
#include "hearthmap/heat_field.h"

namespace hearthmap {

bool point_in_polygon(const Polygon& polygon, MapPoint p) {
  bool inside = false;
  const std::size_t n = polygon.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const MapPoint a = polygon[i];
    const MapPoint b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y)) {
      const double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (p.x < x) inside = !inside;
    }
  }
  return inside;
}

RoomMask build_room_mask(const Config& config) {
  RoomMask m;
  m.cols = config.heat.grid_cols;
  m.rows = config.heat.grid_rows;
  m.room_index.assign(static_cast<std::size_t>(m.cols) * static_cast<std::size_t>(m.rows),
                      RoomMask::kOutside);
  for (std::int32_t r = 0; r < m.rows; ++r) {
    for (std::int32_t c = 0; c < m.cols; ++c) {
      const MapPoint p{(c + 0.5) / m.cols, (r + 0.5) / m.rows};
      for (std::size_t i = 0; i < config.rooms.size(); ++i) {
        if (point_in_polygon(config.rooms[i].polygon, p)) {
          m.room_index[m.index(c, r)] = static_cast<std::int16_t>(i);
          break;
        }
      }
    }
  }
  return m;
}

Rgb888 colour_for(const Band& band, double value, const Palette& palette) {
  if (!std::isfinite(value)) return palette.neutral;
  if (value < band.comfort_min) {
    if (band.comfort_min <= band.scale_min) return palette.neutral;
    return lerp(palette.neutral, palette.cold,
                (band.comfort_min - value) / (band.comfort_min - band.scale_min));
  }
  if (value > band.comfort_max) {
    if (band.scale_max <= band.comfort_max) return palette.neutral;
    return lerp(palette.neutral, palette.hot,
                (value - band.comfort_max) / (band.scale_max - band.comfort_max));
  }
  return palette.neutral;
}

Rgb565 ColourLut::lookup(float value) const {
  if (entries.empty() || !std::isfinite(value)) return 0;
  const float n = static_cast<float>(entries.size() - 1);
  float t = (value - lo) / (hi - lo) * n;
  t = t < 0.0F ? 0.0F : (t > n ? n : t);
  return entries[static_cast<std::size_t>(std::lround(t))];
}

void build_colour_lut(const Band& band, const Palette& palette, std::size_t size, ColourLut& out) {
  if (size < 2) size = 2;
  out.lo = static_cast<float>(band.scale_min);
  out.hi = static_cast<float>(band.scale_max);
  out.entries.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    const double v = band.scale_min + (band.scale_max - band.scale_min) * static_cast<double>(i) /
                                          static_cast<double>(size - 1);
    out.entries[i] = colour565_for(band, v, palette);
  }
}

ColourLut build_colour_lut(const Band& band, const Palette& palette, std::size_t size) {
  ColourLut lut;
  build_colour_lut(band, palette, size, lut);
  return lut;
}

void build_room_mask(const Config& config, RoomMask& out) { out = build_room_mask(config); }

}  // namespace hearthmap
