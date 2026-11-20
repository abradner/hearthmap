// heat_image.h — a HeatField's cells upscaled into the RGB565 map image.
//
// Colour is resolved once per heat cell by the caller (core's colour scale), never per screen
// pixel; this file only blends the cell colours up to pixels, inside each room's own outline:
//
//   pixel outside every room   -> theme::kBackground
//   pixel in an outdoor room   -> theme::kOutdoorFill (outdoor rooms get no heat colour)
//   pixel in an indoor room    -> bilinear blend of the neighbouring Value cells of that same
//                                 room (heat never crosses a wall, D-005), in integer arithmetic
//   ... with no Value cell     -> the "no data" stripes (theme::kNoDataA / kNoDataB)
//
// All per-pixel work is integer, so the image is bit-identical on every platform (the golden
// images depend on it). The rasterizer allocates only in its constructor; render() writes into
// caller-owned storage and allocates nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"

namespace hearthmap::ui {

class HeatRasterizer {
 public:
  /// Precomputes which room owns each pixel of config.canvas.map_area (first room in config
  /// order whose polygon contains the pixel centre, core's point_in_polygon()) and each grid
  /// cell (core's build_room_mask()). `config` need not outlive the rasterizer.
  explicit HeatRasterizer(const Config& config);

  std::int32_t width() const { return width_; }
  std::int32_t height() const { return height_; }
  /// Pixels in the image: width() * height(). The caller's buffer holds this many Rgb565.
  std::size_t pixel_count() const {
    return static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  }
  /// Bytes of working memory the rasterizer itself holds (for device budgeting).
  std::size_t working_bytes() const;

  /// Write the image for `field` into `out` (pixel_count() entries, row-major, stride width()).
  /// `cell_colours` holds one colour per field cell, row-major like field.cells; only entries
  /// of Value cells are read. `field` must have the grid size of the config this was built from;
  /// a mismatched field draws every room as "no data" rather than reading out of bounds.
  void render(const HeatField& field, const std::vector<Rgb565>& cell_colours, Rgb565* out) const;

 private:
  static constexpr std::uint8_t kNoRoom = 0xFF;

  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
  std::int32_t cols_ = 0;
  std::int32_t rows_ = 0;
  std::vector<std::uint8_t> pixel_room_;  ///< Per pixel: room index, or kNoRoom.
  std::vector<std::uint8_t> cell_room_;   ///< Per cell: room index, or kNoRoom.
  std::vector<bool> room_outdoor_;
  // Bilinear tables, per column and per row: the two cells either side and the weight (0..256)
  // of the second.
  std::vector<std::int32_t> col0_, col1_, row0_, row1_;
  std::vector<std::int32_t> wx_, wy_;
};

/// One colour per cell of `field` under `band`, written into `out` (resized to the cell count;
/// no allocation once it has that size). Non-Value cells get palette.neutral; the rasterizer
/// never reads them. Uses core's colour scale.
void colour_cells(const HeatField& field, const Band& band, const Palette& palette,
                  std::vector<Rgb565>& out);

}  // namespace hearthmap::ui
