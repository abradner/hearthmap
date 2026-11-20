#include "hearthmap/ui/heat_image.h"

#include <algorithm>

#include "hearthmap/geometry.h"
#include "hearthmap/ui/theme.h"

namespace hearthmap::ui {

namespace {

constexpr Rgb565 hex565(std::uint32_t rgb) {
  return to_rgb565(Rgb888{static_cast<std::uint8_t>((rgb >> 16U) & 0xFFU),
                          static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU),
                          static_cast<std::uint8_t>(rgb & 0xFFU)});
}

constexpr Rgb565 kBackground565 = hex565(theme::kBackground);
constexpr Rgb565 kOutdoor565 = hex565(theme::kOutdoorFill);
constexpr Rgb565 kNoDataA565 = hex565(theme::kNoDataA);
constexpr Rgb565 kNoDataB565 = hex565(theme::kNoDataB);

// Bilinear table along one axis: pixel p of `pixels` samples cell coordinate
// (p + 0.5) * cells / pixels - 0.5, i.e. cell centres sit at (c + 0.5) * pixels / cells.
// Exact rational arithmetic in 64-bit integers, so the table is identical everywhere.
void build_axis(std::int32_t pixels, std::int32_t cells, std::vector<std::int32_t>& c0,
                std::vector<std::int32_t>& c1, std::vector<std::int32_t>& w) {
  const auto n = static_cast<std::size_t>(pixels);
  c0.assign(n, 0);
  c1.assign(n, 0);
  w.assign(n, 0);
  for (std::int32_t p = 0; p < pixels; ++p) {
    // Cell coordinate scaled by 2 * pixels: (2p + 1) * cells - pixels.
    const std::int64_t num = static_cast<std::int64_t>(2 * p + 1) * cells - pixels;
    const std::int64_t den = 2LL * pixels;
    const auto i = static_cast<std::size_t>(p);
    if (num <= 0) {
      c0[i] = 0;
      c1[i] = 0;
      w[i] = 0;
      continue;
    }
    std::int64_t base = num / den;
    std::int64_t frac = ((num - base * den) * 256 + den / 2) / den;
    if (frac == 256) {
      ++base;
      frac = 0;
    }
    if (base >= cells - 1) {
      c0[i] = cells - 1;
      c1[i] = cells - 1;
      w[i] = 0;
      continue;
    }
    c0[i] = static_cast<std::int32_t>(base);
    c1[i] = static_cast<std::int32_t>(base + 1);
    w[i] = static_cast<std::int32_t>(frac);
  }
}

}  // namespace

HeatRasterizer::HeatRasterizer(const Config& config)
    : width_(config.canvas.map_area.width),
      height_(config.canvas.map_area.height),
      cols_(config.heat.grid_cols),
      rows_(config.heat.grid_rows) {
  if (width_ < 0) width_ = 0;
  if (height_ < 0) height_ = 0;
  room_outdoor_.reserve(config.rooms.size());
  for (const auto& room : config.rooms) room_outdoor_.push_back(room.outdoor);

  // Which room owns each pixel centre: first in config order (config.h, Config::rooms).
  const CanvasRect& area = config.canvas.map_area;
  pixel_room_.assign(pixel_count(), kNoRoom);
  for (std::int32_t y = 0; y < height_; ++y) {
    for (std::int32_t x = 0; x < width_; ++x) {
      const MapPoint p = to_map(area, CanvasPoint{area.x + x + 0.5, area.y + y + 0.5});
      for (std::size_t r = 0; r < config.rooms.size() && r < kNoRoom; ++r) {
        if (point_in_polygon(config.rooms[r].polygon, p)) {
          pixel_room_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                      static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(r);
          break;
        }
      }
    }
  }

  const RoomMask mask = build_room_mask(config);
  cell_room_.assign(mask.room_index.size(), kNoRoom);
  for (std::size_t i = 0; i < mask.room_index.size(); ++i) {
    const std::int16_t r = mask.room_index[i];
    if (r >= 0 && r < kNoRoom) cell_room_[i] = static_cast<std::uint8_t>(r);
  }
  cols_ = mask.cols;
  rows_ = mask.rows;

  build_axis(width_, cols_, col0_, col1_, wx_);
  build_axis(height_, rows_, row0_, row1_, wy_);
}

std::size_t HeatRasterizer::working_bytes() const {
  return pixel_room_.size() + cell_room_.size() +
         sizeof(std::int32_t) *
             (col0_.size() + col1_.size() + wx_.size() + row0_.size() + row1_.size() + wy_.size());
}

void HeatRasterizer::render(const HeatField& field, const std::vector<Rgb565>& cell_colours,
                            Rgb565* out) const {
  const std::size_t cell_count = static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_);
  const bool field_ok = field.cols == cols_ && field.rows == rows_ &&
                        field.cells.size() == cell_count && cell_colours.size() == cell_count;
  const auto ucols = static_cast<std::size_t>(cols_);

  for (std::int32_t y = 0; y < height_; ++y) {
    const auto yi = static_cast<std::size_t>(y);
    const std::int32_t wy = wy_[yi];
    const std::size_t rows_idx[2] = {static_cast<std::size_t>(row0_[yi]),
                                     static_cast<std::size_t>(row1_[yi])};
    const std::int32_t wys[2] = {256 - wy, wy};
    for (std::int32_t x = 0; x < width_; ++x) {
      const auto xi = static_cast<std::size_t>(x);
      const std::size_t idx = yi * static_cast<std::size_t>(width_) + xi;
      const std::uint8_t room = pixel_room_[idx];
      Rgb565& px = out[idx];
      if (room == kNoRoom) {
        px = kBackground565;
        continue;
      }
      if (room_outdoor_[room]) {
        px = kOutdoor565;
        continue;
      }

      std::uint32_t sum_w = 0;
      std::uint32_t sum_r = 0;
      std::uint32_t sum_g = 0;
      std::uint32_t sum_b = 0;
      if (field_ok) {
        const std::int32_t wx = wx_[xi];
        const std::size_t cols_idx[2] = {static_cast<std::size_t>(col0_[xi]),
                                         static_cast<std::size_t>(col1_[xi])};
        const std::int32_t wxs[2] = {256 - wx, wx};
        for (int j = 0; j < 2; ++j) {
          for (int i = 0; i < 2; ++i) {
            const std::uint32_t w = static_cast<std::uint32_t>(wxs[i] * wys[j]);
            if (w == 0) continue;
            const std::size_t c = rows_idx[j] * ucols + cols_idx[i];
            if (cell_room_[c] != room || field.cells[c].state != CellState::Value) continue;
            const Rgb565 col = cell_colours[c];
            sum_w += w;
            sum_r += w * ((col >> 11U) & 0x1FU);
            sum_g += w * ((col >> 5U) & 0x3FU);
            sum_b += w * (col & 0x1FU);
          }
        }
        if (sum_w == 0) {
          // Near a wall all four neighbours can belong to other rooms (or carry no weight).
          // Take the nearest Value cell of this room within two cells, if any: a measured room
          // never shows "no data" slivers along its walls.
          const std::int32_t cc = wx_[xi] < 128 ? col0_[xi] : col1_[xi];
          const std::int32_t cr = wy < 128 ? row0_[yi] : row1_[yi];
          std::int32_t best = -1;
          std::int32_t best_d = 1 << 30;
          for (std::int32_t dr = -2; dr <= 2; ++dr) {
            for (std::int32_t dc = -2; dc <= 2; ++dc) {
              const std::int32_t r2 = cr + dr;
              const std::int32_t c2 = cc + dc;
              if (r2 < 0 || c2 < 0 || r2 >= rows_ || c2 >= cols_) continue;
              const std::size_t c =
                  static_cast<std::size_t>(r2) * ucols + static_cast<std::size_t>(c2);
              if (cell_room_[c] != room || field.cells[c].state != CellState::Value) continue;
              const std::int32_t d = dr * dr + dc * dc;
              if (d < best_d) {
                best_d = d;
                best = static_cast<std::int32_t>(c);
              }
            }
          }
          if (best >= 0) {
            px = cell_colours[static_cast<std::size_t>(best)];
            continue;
          }
        }
      }
      if (sum_w == 0) {
        px = (((x + y) / theme::kNoDataStripePx) % 2) == 0 ? kNoDataA565 : kNoDataB565;
        continue;
      }
      const std::uint32_t half = sum_w / 2;
      const std::uint32_t r = (sum_r + half) / sum_w;
      const std::uint32_t g = (sum_g + half) / sum_w;
      const std::uint32_t b = (sum_b + half) / sum_w;
      px = static_cast<Rgb565>((r << 11U) | (g << 5U) | b);
    }
  }
}

void colour_cells(const HeatField& field, const Band& band, const Palette& palette,
                  std::vector<Rgb565>& out) {
  out.resize(field.cells.size());
  const Rgb565 neutral = to_rgb565(palette.neutral);
  for (std::size_t i = 0; i < field.cells.size(); ++i) {
    const HeatCell& cell = field.cells[i];
    out[i] = cell.state == CellState::Value
                 ? colour565_for(band, static_cast<double>(cell.value), palette)
                 : neutral;
  }
}

}  // namespace hearthmap::ui
