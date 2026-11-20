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
  /// order whose polygon contains the pixel centre, exactly core's point_in_polygon()) and each
  /// grid cell (exactly core's build_room_mask()). `config` need not outlive the rasterizer.
  explicit HeatRasterizer(const Config& config);

  /// Tag for the two-phase form below.
  struct Deferred {};

  /// Two-phase construction for embedders that must not block (the device's boot,
  /// docs/decisions.md D-038): sizes everything and returns at once; room ownership is then
  /// computed by build_rows() calls, a few rows at a time. The one-argument constructor is this
  /// plus build_rows(config, all rows), so the result is identical.
  HeatRasterizer(const Config& config, Deferred);

  /// Compute room ownership for up to `max_rows` more rows: the heat grid's rows first, then the
  /// image's pixel rows. Returns ready(). `config` must be the one the rasterizer was
  /// constructed with. Each row costs double-precision work for the room edges that cross it
  /// plus integer work per pixel (core's RoomScanner); nothing is per pixel per room.
  bool build_rows(const Config& config, std::int32_t max_rows);

  /// True once every row is built. render() before that draws unbuilt rows as background.
  bool ready() const { return next_row_ >= rows_ + height_; }

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

  /// render() for pixel rows [row_begin, row_end) only (clamped to the image), so an embedder
  /// can spread one image over several calls; every row over all calls gives exactly render().
  void render_rows(const HeatField& field, const std::vector<Rgb565>& cell_colours, Rgb565* out,
                   std::int32_t row_begin, std::int32_t row_end) const;

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
  // Deferred build state.
  RoomScanner scanner_;
  std::vector<std::int16_t> scan_row_;  ///< One row of scanner output.
  std::int32_t next_row_ = 0;           ///< 0..rows_ grid rows, then rows_..rows_+height_ pixels.
};

/// One colour per cell of `field` through core's ColourLut (built once per band), written into
/// `out` (resized to the cell count; no allocation once it has that size). Non-Value cells get
/// 0; the rasterizer never reads them.
void colour_cells(const HeatField& field, const ColourLut& lut, std::vector<Rgb565>& out);

}  // namespace hearthmap::ui
