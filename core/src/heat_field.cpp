// heat_field.cpp — room mask and room-local inverse-distance interpolation (heat_field.h).
//
// Cost, with C = cols * rows cells, R rooms, V = total polygon vertices, S samples and k the
// number of usable samples in a cell's own room:
//   build_room_mask:    O(C * R) bounding-box checks, plus O(vertices) for each room whose box
//                       holds the cell centre. Worst case O(C * V).
//   compute_heat_field: O(C * S) room-index comparisons plus O(C * k) weight evaluations. Each
//                       cell scans the sample list for its own room rather than using a
//                       per-call grouping table: with tens of samples the comparison is cheaper
//                       than the allocation a table would need, and the call then allocates
//                       nothing once `out` has held a field this size (D-022).
// Interpolation is float (the ESP32-P4 has a single-precision FPU only). Iteration order is
// fixed (cells row-major, samples in input order), so the same inputs give bit-identical output.
// Powers 1..8 that are integers avoid powf (whose rounding differs between C libraries), and
// core is built with -ffp-contract=off (D-024), so for those powers the output is also identical
// across IEEE-754 platforms.
#include "hearthmap/heat_field.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace hearthmap {

namespace {

// Rooms beyond what an int16 room index can name are never assigned (RoomMask stores int16).
std::size_t nameable_room_count(const Config& config) {
  constexpr auto kMaxRooms = static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max());
  return config.rooms.size() < kMaxRooms ? config.rooms.size() : kMaxRooms;
}

// 1 / d^power from the squared distance.
class InverseDistance {
 public:
  explicit InverseDistance(double power)
      : half_power_(static_cast<float>(power * 0.5)),
        integer_power_(power >= 1.0 && power <= 8.0 && std::floor(power) == power
                           ? static_cast<int>(power)
                           : 0) {}

  float weight(float d2) const {
    switch (integer_power_) {
      case 0: return 1.0F / std::pow(d2, half_power_);  // powf
      case 2: return 1.0F / d2;                         // the common case: multiply-only
      default: {
        float denom = 1.0F;
        for (int i = 0; i < integer_power_ / 2; ++i) denom *= d2;
        if (integer_power_ % 2 != 0) denom *= std::sqrt(d2);
        return 1.0F / denom;
      }
    }
  }

 private:
  float half_power_;
  int integer_power_;
};

// Squared canvas-pixel distance under which a cell takes a sample's value exactly
// (heat_field.h: d < 0.5 pixel).
constexpr float kCoincidentD2 = 0.25F;

bool usable(const HeatSample& s, std::int16_t room) {
  return s.room_index == room && std::isfinite(s.position.x) && std::isfinite(s.position.y) &&
         std::isfinite(s.value);
}

}  // namespace

void build_room_mask(const Config& config, RoomMask& out) {
  const std::int32_t cols = config.heat.grid_cols;
  const std::int32_t rows = config.heat.grid_rows;
  if (cols <= 0 || rows <= 0) {  // nothing to tile; validate() rejects this config
    out.cols = 0;
    out.rows = 0;
    out.room_index.clear();
    return;
  }
  out.cols = cols;
  out.rows = rows;
  out.room_index.assign(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows),
                        RoomMask::kOutside);

  const std::size_t room_count = nameable_room_count(config);
  for (std::size_t i = 0; i < room_count; ++i) {
    const Polygon& polygon = config.rooms[i].polygon;
    if (polygon.size() < 3) continue;  // encloses nothing
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    double max_x = -min_x;
    double max_y = -min_x;
    for (const MapPoint& v : polygon) {
      if (v.x < min_x) min_x = v.x;
      if (v.y < min_y) min_y = v.y;
      if (v.x > max_x) max_x = v.x;
      if (v.y > max_y) max_y = v.y;
    }
    // Rooms are painted in config order onto unclaimed cells only, so the first room wins.
    for (std::int32_t r = 0; r < rows; ++r) {
      const double y = (r + 0.5) / rows;
      if (y < min_y || y > max_y) continue;  // bounding box: cheap rejection
      for (std::int32_t c = 0; c < cols; ++c) {
        const std::size_t at = out.index(c, r);
        if (out.room_index[at] != RoomMask::kOutside) continue;
        const MapPoint centre{(c + 0.5) / cols, y};
        if (centre.x < min_x || centre.x > max_x) continue;  // bounding box: cheap rejection
        if (point_in_polygon(polygon, centre)) {
          out.room_index[at] = static_cast<std::int16_t>(i);
        }
      }
    }
  }
}

RoomMask build_room_mask(const Config& config) {
  RoomMask mask;
  build_room_mask(config, mask);
  return mask;
}

void compute_heat_field(const Config& config, const RoomMask& mask,
                        const std::vector<HeatSample>& samples, HeatField& out) {
  const std::size_t cell_count =
      mask.cols > 0 && mask.rows > 0
          ? static_cast<std::size_t>(mask.cols) * static_cast<std::size_t>(mask.rows)
          : 0;
  if (cell_count == 0 || mask.room_index.size() != cell_count) {  // not a mask: refuse
    out.cols = 0;
    out.rows = 0;
    out.cells.clear();
    return;
  }
  out.cols = mask.cols;
  out.rows = mask.rows;
  out.cells.assign(cell_count, HeatCell{});

  const std::size_t room_count = config.rooms.size();
  const auto width = static_cast<float>(config.canvas.map_area.width);
  const auto height = static_cast<float>(config.canvas.map_area.height);
  const auto cols = static_cast<float>(mask.cols);
  const auto rows = static_cast<float>(mask.rows);
  const InverseDistance idw(config.heat.idw_power);

  for (std::int32_t r = 0; r < mask.rows; ++r) {
    const float cy = ((static_cast<float>(r) + 0.5F) / rows) * height;
    for (std::int32_t c = 0; c < mask.cols; ++c) {
      const std::size_t at = mask.index(c, r);
      const std::int16_t room = mask.room_index[at];
      HeatCell& cell = out.cells[at];
      // Outside: not in a room, a room this config lacks (mask from another config), or an
      // outdoor room, which gets no heat colour (D-006).
      if (room < 0 || static_cast<std::size_t>(room) >= room_count) continue;
      if (config.rooms[static_cast<std::size_t>(room)].outdoor) continue;
      const float cx = ((static_cast<float>(c) + 0.5F) / cols) * width;
      // Accumulated relative to the room's first sample value: sum(w * (v - base)) / sum(w) +
      // base is the same weighted mean, but a single sample (or several equal ones) gives its
      // value exactly rather than w * v / w, which float rounding can move by an ulp; centring
      // also keeps more of float's precision for the differences that matter.
      float base = 0.0F;
      float weighted = 0.0F;
      float total = 0.0F;
      bool any = false;
      bool coincident = false;
      for (const HeatSample& s : samples) {
        if (!usable(s, room)) continue;
        if (!any) base = s.value;
        any = true;
        const float dx = cx - s.position.x * width;
        const float dy = cy - s.position.y * height;
        const float d2 = dx * dx + dy * dy;
        if (d2 < kCoincidentD2) {
          cell.value = s.value;
          coincident = true;
          break;  // the first coincident sample in input order wins
        }
        const float w = idw.weight(d2);
        weighted += w * (s.value - base);
        total += w;
      }
      if (!any) {
        cell.state = CellState::NoData;
        continue;
      }
      cell.state = CellState::Value;
      if (!coincident) cell.value = base + weighted / total;
    }
  }
}

HeatField compute_heat_field(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples) {
  HeatField field;
  compute_heat_field(config, mask, samples, field);
  return field;
}

}  // namespace hearthmap
