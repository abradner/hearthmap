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

/// Build the mask for `config` at config.heat.grid_cols x grid_rows.
/// Implemented in core (PR 3).
RoomMask build_room_mask(const Config& config);

/// One point measurement feeding a field.
struct HeatSample {
  MapPoint position;
  std::int16_t room_index = RoomMask::kOutside;  ///< Index into Config::rooms.
  double value = 0.0;                            ///< In the layer's value domain.
};

enum class CellState : std::uint8_t {
  Outside,  ///< Not in any room: draw nothing (floor-plan background shows through).
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

/// Interpolate `samples` over `mask`.
///
/// Rules (docs/decisions.md D-005):
/// - Room-local: a cell in room R takes influence only from samples whose room_index == R.
///   Heat does not bleed through walls, and an unmeasured room shows NoData rather than a
///   value borrowed from its neighbours. Outdoor rooms are rooms like any other here.
/// - Weighting: w = 1 / d^power, d = distance from cell centre to sample in canvas pixels
///   (map space scaled by config.canvas.map_area width and height), power = config.heat
///   .idw_power. value = sum(w * v) / sum(w).
/// - Coincidence: if d < 0.5 pixel for any sample, the cell takes that sample's value exactly
///   (the first such sample in `samples` order).
/// - Samples with room_index == kOutside are ignored.
/// - Output cells are Outside where mask says so, NoData where the room has no sample, Value
///   otherwise.
/// `mask` must have been built from `config`.
/// Implemented in core (PR 3).
HeatField compute_heat_field(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples);

}  // namespace hearthmap
