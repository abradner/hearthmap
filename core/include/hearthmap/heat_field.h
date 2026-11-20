// heat_field.h — inverse-distance interpolation on a coarse grid, masked to room polygons.
//
// One field per layer. The field knows nothing about readings, freshness or severity: it takes
// samples (position, room, value) and produces a grid. view_model.h's heat_samples() turns
// readings into samples; colour_scale.h turns values into pixels; ui_lvgl upscales the grid
// into an image. Splitting it this way keeps each step testable alone.
//
// Performance contract (docs/decisions.md D-022; the ESP32-P4 has a single-precision FPU only):
// per-cell arithmetic is float, never double; colour is resolved per cell through a ColourLut
// (colour_scale.h), never per screen pixel; the reuse forms below write into caller-owned
// storage and do not allocate once that storage has reached the grid's size.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/geometry.h"

namespace hearthmap {

/// Which room each grid cell belongs to. Depends only on the config, so build it once per
/// config and reuse it for every layer and every refresh.
///
/// Grid geometry: `cols` x `rows` cells tile the map area exactly. Cell (c, r) has its centre
/// at map point ((c + 0.5) / cols, (r + 0.5) / rows). Row 0 is the top row. Storage is
/// row-major: index = r * cols + c.
struct RoomMask {
  std::int32_t cols = 0;
  std::int32_t rows = 0;
  /// Per cell: index into Config::rooms of the first room (in config order) whose polygon
  /// contains the cell centre (point_in_polygon()), or kOutside. Outdoor rooms are recorded
  /// like any other room, so the UI can draw their outdoor treatment; it is
  /// compute_heat_field() that gives them no colour.
  std::vector<std::int16_t> room_index;

  static constexpr std::int16_t kOutside = -1;

  std::size_t index(std::int32_t col, std::int32_t row) const {
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
           static_cast<std::size_t>(col);
  }
};

/// Which room owns each centre of a grid laid over map space, computed one row at a time.
///
/// For a `cols` x `rows` grid, cell (c, r) has its centre at map point ((c + 0.5) / cols,
/// (r + 0.5) / rows), and scan_row() gives every cell of row r the index of the first room in
/// config order whose polygon contains that centre, or RoomMask::kOutside. The answer is exactly
/// point_in_polygon()'s for each centre, by construction: both evaluate the same edge arithmetic,
/// once per edge per row here instead of once per edge per point.
///
/// Cost per row: double-precision work for the edges that cross the row only (O(E) worst case,
/// E = total vertices over all rooms, typically a few per room) plus O(cols) integer fills. That
/// is what makes the device's boot affordable: point_in_polygon() per pixel was O(pixels x E) in
/// software double precision on the ESP32-P4 (docs/decisions.md D-038). Splitting the rows across
/// calls lets an embedder spread the work and stay responsive. The scanner borrows nothing: it
/// copies the rooms' edges at construction (allocates), and scan_row() allocates nothing once its
/// scratch has grown to the busiest row.
class RoomScanner {
 public:
  RoomScanner() = default;
  explicit RoomScanner(const Config& config);

  /// Row `row` of a `cols` x `rows` grid into `out[0 .. cols)`. Does nothing for cols <= 0,
  /// rows <= 0 or a row outside 0..rows-1.
  void scan_row(std::int32_t cols, std::int32_t rows, std::int32_t row, std::int16_t* out);

  /// Total edges held (for budgeting a row's worst case).
  std::size_t edge_count() const { return edges_.size(); }

 private:
  struct Edge {
    double y_lo;  ///< the endpoints' y, lower first (spans() is order-free)
    double y_hi;
    double a_x, a_y, dx, dy;  ///< canonical form (core/src/polygon_edges.h)
  };
  struct RoomEdges {
    std::size_t first = 0;  ///< into edges_
    std::size_t count = 0;  ///< 0 for rooms that enclose nothing (< 3 vertices, collinear)
    double y_min = 0.0;
    double y_max = 0.0;
  };
  std::vector<Edge> edges_;
  std::vector<RoomEdges> rooms_;
  std::vector<std::int32_t> thresholds_;  ///< scratch: per crossing, cells left of it
};

/// Build the mask for `config` at config.heat.grid_cols x grid_rows into `out`, reusing its
/// storage. Not per-frame (the mask depends only on the config), but cheap to redo.
/// A non-positive grid size gives an empty mask (cols = rows = 0). Rooms past index 32766
/// (beyond int16) are never assigned. Cost: O(cells * rooms) bounding-box checks plus a
/// point_in_polygon() per candidate room.
void build_room_mask(const Config& config, RoomMask& out);

/// Returning form of build_room_mask(); allocates.
RoomMask build_room_mask(const Config& config);

/// One point measurement feeding a field. Single precision throughout (D-022).
struct HeatSample {
  MapPointF position;
  std::int16_t room_index = RoomMask::kOutside;  ///< Index into Config::rooms.
  float value = 0.0F;                            ///< In the layer's value domain.
};

enum class CellState : std::uint8_t {
  Outside,  ///< Not in any room, or in an outdoor room: no heat colour. The UI draws outdoor
            ///< rooms in their outdoor treatment (from RoomMask), and nothing elsewhere.
  NoData,   ///< In a room that has no sample: draw the "no data" treatment, never a guess.
  Value,    ///< Interpolated; `value` is meaningful.
};

struct HeatCell {
  CellState state = CellState::Outside;
  float value = 0.0F;
};

struct HeatField {
  std::int32_t cols = 0;
  std::int32_t rows = 0;
  std::vector<HeatCell> cells;  ///< Row-major, same geometry as RoomMask.
};

/// Interpolate `samples` over `mask`, writing into `out` (resized to the mask's geometry,
/// reusing its storage: no allocation once out.cells has held mask.cols * mask.rows cells).
///
/// Rules (docs/decisions.md D-005):
/// - Room-local: a cell in room R takes influence only from samples whose room_index == R.
///   Heat does not bleed through walls, and an unmeasured room shows NoData rather than a
///   value borrowed from its neighbours.
/// - Outdoor rooms get no heat colour (D-006): their cells are Outside, and samples whose room
///   is outdoor are ignored. Their pins still show values; that is view_model.h's business.
/// - Weighting: w = 1 / d^power, d = distance from cell centre to sample in canvas pixels
///   (map space scaled by config.canvas.map_area width and height), power = config.heat
///   .idw_power. value = sum(w * v) / sum(w), evaluated as base + sum(w * (v - base)) / sum(w)
///   with base = the room's first usable sample value, so one sample yields its value exactly,
///   and with values pre-scaled by 2^-20 so that v - base cannot overflow for finite samples
///   (scaling by a power of two is exact for normal floats, so ordinary results are unchanged).
///   A cell whose result is still not finite (every weight underflowed: 0/0) is NoData.
///   All in float. When idw_power == 2 exactly, w is
///   1 / (dx*dx + dy*dy): multiply-only, no sqrt, no powf. Other integer powers 1..8 use
///   multiplication and at most one sqrtf; non-integer powers use powf.
/// - Coincidence: a cell takes a sample's value exactly only when that sample is in the cell's
///   own room and d < 0.5 pixel (the first such sample in `samples` order). A sample in another
///   room never sets a cell, however close.
/// - Samples with room_index == kOutside, or out of range for config.rooms, or with a
///   non-finite position or value, are ignored. A room whose samples are all ignored is NoData.
/// - Output cells are Outside where the mask says kOutside or the room is outdoor, NoData where
///   the room has no usable sample, Value otherwise.
/// `mask` must have been built from `config`. If it is not a mask at all (cols or rows <= 0, or
/// room_index.size() != cols * rows) `out` becomes an empty field (cols = rows = 0); a mask cell
/// naming a room `config` does not have is Outside.
/// Cost: O(cells * samples) room-index comparisons plus O(cells * samples in the cell's room)
/// weight evaluations. Same inputs give bit-identical cells (docs/decisions.md D-024).
void compute_heat_field(const Config& config, const RoomMask& mask,
                        const std::vector<HeatSample>& samples, HeatField& out);

/// Row-by-row form of compute_heat_field(), for embedders that spread the work: begin_heat_field()
/// sizes and clears `out` for `mask` (false, with `out` emptied, when `mask` is not a valid mask:
/// exactly compute_heat_field()'s refusal), then compute_heat_field_rows() fills rows
/// [row_begin, row_end) of it, clamped to the grid. Calling it over every row gives exactly
/// compute_heat_field()'s result; neither call allocates once `out` has held a field this size.
bool begin_heat_field(const RoomMask& mask, HeatField& out);
void compute_heat_field_rows(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples, std::int32_t row_begin,
                             std::int32_t row_end, HeatField& out);

/// Returning form of compute_heat_field(); allocates. Tests and the host use it; the device
/// keeps one HeatField per layer and calls the reuse form.
HeatField compute_heat_field(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples);

}  // namespace hearthmap
