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
// images depend on it). The rasterizer allocates only in its constructor, and only through the
// memory port (hearthmap/ui/memory.h, D-064): its per-pixel room index and bilinear tables are
// bulk storage. render() writes into caller-owned storage and allocates nothing. Its RoomScanner
// is core's and keeps core's own small vectors (the rooms' edges) on the default heap.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/ui/memory.h"

namespace hearthmap::ui {

/// One colour per heat cell, row-major, as colour_cells() writes them: a view of the caller's
/// storage (a std::vector or a BulkVector), never a copy.
class CellColours {
 public:
  CellColours() = default;
  template <class Container>
  CellColours(const Container& c)  // NOLINT: a view converts implicitly, as a span does
      : p_(c.data()), n_(c.size()) {}
  const Rgb565* data() const { return p_; }
  std::size_t size() const { return n_; }
  const Rgb565& operator[](std::size_t i) const { return p_[i]; }

 private:
  const Rgb565* p_ = nullptr;
  std::size_t n_ = 0;
};

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
  void render(const HeatField& field, CellColours cell_colours, Rgb565* out) const;

  /// What render_rows() needs from `field` beyond its cells: whether it fits this rasterizer,
  /// and each room's first Value cell (the fallback for pixels far from any of their room's
  /// Value cells). One pass over every cell, so compute it once per image with prepare(), not
  /// once per slice of rows.
  struct Prepared {
    static constexpr std::size_t kRooms = 0xFF;
    bool field_ok = false;
    std::array<std::int32_t, kRooms> first_value{};
  };
  Prepared prepare(const HeatField& field, CellColours cell_colours) const;

  /// render() for pixel rows [row_begin, row_end) only (clamped to the image), so an embedder
  /// can spread one image over several calls; every row over all calls gives exactly render().
  /// `prepared` must come from prepare() with the same field and colours.
  void render_rows(const Prepared& prepared, const HeatField& field, CellColours cell_colours,
                   Rgb565* out, std::int32_t row_begin, std::int32_t row_end) const;
  /// The same, preparing first: a pass over every cell per call.
  void render_rows(const HeatField& field, CellColours cell_colours, Rgb565* out,
                   std::int32_t row_begin, std::int32_t row_end) const {
    render_rows(prepare(field, cell_colours), field, cell_colours, out, row_begin, row_end);
  }

 private:
  static constexpr std::uint8_t kNoRoom = 0xFF;
  static_assert(Prepared::kRooms == kNoRoom, "one first-value slot per room index");

  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
  std::int32_t cols_ = 0;
  std::int32_t rows_ = 0;
  BulkVector<std::uint8_t> pixel_room_;  ///< Per pixel: room index, or kNoRoom.
  BulkVector<std::uint8_t> cell_room_;   ///< Per cell: room index, or kNoRoom.
  BulkVector<std::uint8_t> room_outdoor_;
  // Bilinear tables, per column and per row: the two cells either side and the weight (0..256)
  // of the second.
  BulkVector<std::int32_t> col0_, col1_, row0_, row1_;
  BulkVector<std::int32_t> wx_, wy_;
  // Deferred build state.
  RoomScanner scanner_;
  BulkVector<std::int16_t> scan_row_;  ///< One row of scanner output.
  std::int32_t next_row_ = 0;          ///< 0..rows_ grid rows, then rows_..rows_+height_ pixels.
};

/// One colour per cell of `field` through core's ColourLut (built once per band), written into
/// `out` (resized to the cell count; no allocation once it has that size). Non-Value cells get
/// 0; the rasterizer never reads them.
void colour_cells(const HeatField& field, const ColourLut& lut, std::vector<Rgb565>& out);
void colour_cells(const HeatField& field, const ColourLut& lut, BulkVector<Rgb565>& out);

}  // namespace hearthmap::ui
