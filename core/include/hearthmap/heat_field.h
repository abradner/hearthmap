// heat_field.h — inverse-distance interpolation on a coarse grid, masked to room polygons.
//
// One field per layer. The field knows nothing about readings, freshness or severity: it takes
// samples (position, room, value) and produces a grid. view_model.h's heat_samples() turns
// readings into samples; colour_scale.h turns values into pixels; ui_lvgl upscales the grid
// into an image. Splitting it this way keeps each step testable alone.
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
  /// contains the cell centre (point_in_polygon()), or kOutside.
  std::vector<std::int16_t> room_index;

  static constexpr std::int16_t kOutside = -1;

  std::size_t index(std::int32_t col, std::int32_t row) const {
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
           static_cast<std::size_t>(col);
  }
};

/// Build the mask for `config` at config.heat.grid_cols x grid_rows, into caller-owned `out`
/// (its storage is reused: no allocation once it has held a mask this size). A non-positive grid
/// size gives an empty mask (cols = rows = 0). Rooms past index 32766 (beyond int16) are never
/// assigned. Outdoor rooms are assigned like any other room (hit-testing needs them); the heat
/// field is what leaves them uncoloured. Cost: O(cells * rooms) bounding-box checks plus a
/// point_in_polygon() per candidate room; build it once per config.
void build_room_mask(const Config& config, RoomMask& out);

/// Returning form of build_room_mask(config, out).
RoomMask build_room_mask(const Config& config);

/// One point measurement feeding a field. `value` is float: the device has a single-precision
/// FPU only (docs/decisions.md, contract rulings R11).
struct HeatSample {
  MapPoint position;
  std::int16_t room_index = RoomMask::kOutside;  ///< Index into Config::rooms.
  float value = 0.0F;                            ///< In the layer's value domain.
};

enum class CellState : std::uint8_t {
  Outside,  ///< Not in any indoor room: draw nothing here (floor plan or outdoor treatment).
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

/// Interpolate `samples` over `mask`, into caller-owned `out` (its storage is reused: once it
/// has held a field this size, the call allocates nothing).
///
/// Rules (docs/decisions.md D-005):
/// - Room-local: a cell in room R takes influence only from samples whose room_index == R.
///   Heat does not bleed through walls, and an unmeasured room shows NoData rather than a
///   value borrowed from its neighbours.
/// - Outdoor rooms get no heat colour: their cells are Outside (ruling R10).
/// - Weighting: w = 1 / d^power, d = distance from cell centre to sample in canvas pixels
///   (map space scaled by config.canvas.map_area width and height), power = config.heat
///   .idw_power. value = sum(w * v) / sum(w), evaluated as base + sum(w * (v - base)) / sum(w)
///   with base = the room's first usable sample value, so one sample (or several equal ones)
///   yields its value exactly. Arithmetic is float. power == 2 is multiply-only;
///   other integer powers 1..8 use multiplication and at most one sqrtf; other powers use powf.
/// - Coincidence: if d < 0.5 pixel for a sample in the cell's own room, the cell takes that
///   sample's value exactly (the first such sample in `samples` order). Samples in other rooms
///   never count, however close (ruling R4).
/// - Samples with room_index == kOutside are ignored, and so are samples whose room_index names
///   no room in `config` or whose position or value is not finite. A room whose samples are all
///   ignored is NoData.
/// - Output cells are Outside where the mask says so (or names an outdoor room), NoData where the
///   room has no usable sample, Value otherwise.
/// `mask` must have been built from `config`. If it is not a mask at all (cols or rows <= 0, or
/// room_index.size() != cols * rows) the result is an empty field (cols = rows = 0); a mask cell
/// naming a room `config` does not have is Outside.
/// Cost: O(cells * samples) room-index comparisons plus O(cells * samples in the cell's room)
/// weight evaluations. Same inputs give bit-identical cells (docs/decisions.md D-017).
void compute_heat_field(const Config& config, const RoomMask& mask,
                        const std::vector<HeatSample>& samples, HeatField& out);

/// Returning form of compute_heat_field(config, mask, samples, out).
HeatField compute_heat_field(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples);

}  // namespace hearthmap
