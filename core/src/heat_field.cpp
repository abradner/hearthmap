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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "polygon_edges.h"

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

// Scaling for the weighted mean's accumulation (see compute_heat_field): powers of two, exact.
constexpr float kScale = 0x1p-20F;
constexpr float kUnscale = 0x1p20F;

bool usable(const HeatSample& s, std::int16_t room) {
  return s.room_index == room && std::isfinite(s.position.x) && std::isfinite(s.position.y) &&
         std::isfinite(s.value);
}

}  // namespace

namespace {

// Cells of a `cols`-wide row whose centre lies strictly left of x: point_in_polygon() counts an
// edge crossing at x for centre cx exactly when cx < x, and centres rise with c, so that set is
// c in [0, t). Settled on the exact per-point predicate after a cheap estimate.
std::int32_t cells_left_of(double x, std::int32_t cols) {
  if (!(x == x)) return 0;  // NaN: no centre is left of it
  std::int32_t t = 0;
  const double estimate = x * cols - 0.5;
  if (estimate >= cols) {
    t = cols;
  } else if (estimate > 0) {
    t = static_cast<std::int32_t>(estimate);
  }
  while (t > 0 && !((t - 1 + 0.5) / cols < x)) --t;
  while (t < cols && (t + 0.5) / cols < x) ++t;
  return t;
}

// Given each crossing's cells_left_of() threshold, mark room `room` on the cells it owns: cell c
// is inside when it is left of an odd number of crossings, #{t > c}. Earlier rooms keep their
// cells (first in config order wins). Sorts `t` in place.
void fill_row(std::int32_t* t, std::size_t m, std::int32_t cols, std::int16_t room,
              std::int16_t* out) {
  std::sort(t, t + m);
  std::int32_t from = 0;
  for (std::size_t j = 0; j <= m; ++j) {
    const std::int32_t to = j < m ? t[j] : cols;
    if (((m - j) & 1U) != 0) {  // for c in [from, to): #{t > c} = m - j
      for (std::int32_t c = from; c < to; ++c) {
        if (out[c] == RoomMask::kOutside) out[c] = room;
      }
    }
    from = to;
  }
}

// Crossings one row can hold on the stack in build_room_mask(); validate() caps polygons at 64
// vertices, and a polygon with more falls back to per-point tests there.
constexpr std::size_t kStackCrossings = 256;

}  // namespace

RoomScanner::RoomScanner(const Config& config) {
  const std::size_t room_count = nameable_room_count(config);
  rooms_.resize(room_count);
  std::size_t total = 0;
  for (std::size_t i = 0; i < room_count; ++i) total += config.rooms[i].polygon.size();
  edges_.reserve(total);
  for (std::size_t i = 0; i < room_count; ++i) {
    const Polygon& polygon = config.rooms[i].polygon;
    RoomEdges& room = rooms_[i];
    room.first = edges_.size();
    // point_in_polygon() answers "outside" for these everywhere: no edges, no crossings.
    if (polygon.size() < 3 || detail::collinear(polygon)) continue;
    room.y_min = std::numeric_limits<double>::infinity();
    room.y_max = -room.y_min;
    for (std::size_t k = 0, j = polygon.size() - 1; k < polygon.size(); j = k++) {
      const detail::CanonicalEdge e = detail::canonical_edge(polygon[j], polygon[k]);
      edges_.push_back(Edge{e.y_lo, e.y_hi, e.a.x, e.a.y, e.dx, e.dy});
      if (polygon[k].y < room.y_min) room.y_min = polygon[k].y;
      if (polygon[k].y > room.y_max) room.y_max = polygon[k].y;
    }
    room.count = edges_.size() - room.first;
  }
  thresholds_.reserve(total);
}

void RoomScanner::scan_row(std::int32_t cols, std::int32_t rows, std::int32_t row,
                           std::int16_t* out) {
  if (cols <= 0 || rows <= 0 || row < 0 || row >= rows) return;
  std::fill(out, out + cols, RoomMask::kOutside);
  const double y = (row + 0.5) / rows;  // the centre build_room_mask() has always used
  for (std::size_t i = 0; i < rooms_.size(); ++i) {
    const RoomEdges& room = rooms_[i];
    // No edge spans a row outside the room's y range ((y_lo > y) != (y_hi > y) is false).
    if (room.count == 0 || !(y >= room.y_min && y <= room.y_max)) continue;
    thresholds_.clear();
    for (std::size_t k = room.first; k < room.first + room.count; ++k) {
      const Edge& e = edges_[k];
      if ((e.y_lo > y) == (e.y_hi > y)) continue;  // detail::spans(), which is order-free
      const detail::CanonicalEdge ce{{e.a_x, e.a_y}, e.dx, e.dy, e.y_lo, e.y_hi};
      thresholds_.push_back(cells_left_of(detail::crossing_x(ce, y), cols));
    }
    if (!thresholds_.empty()) {
      fill_row(thresholds_.data(), thresholds_.size(), cols, static_cast<std::int16_t>(i), out);
    }
  }
}

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

  // The same scanline as RoomScanner, without its allocations (this reuse form allocates
  // nothing once warm, D-022): rooms in config order, crossings on the stack.
  std::array<std::int32_t, kStackCrossings> t{};
  const std::size_t room_count = nameable_room_count(config);
  for (std::size_t i = 0; i < room_count; ++i) {
    const Polygon& polygon = config.rooms[i].polygon;
    const auto room = static_cast<std::int16_t>(i);
    if (polygon.size() < 3 || detail::collinear(polygon)) continue;
    if (polygon.size() > kStackCrossings) {  // unvalidated config: per-point tests, still exact
      for (std::int32_t r = 0; r < rows; ++r) {
        for (std::int32_t c = 0; c < cols; ++c) {
          std::int16_t& cell = out.room_index[out.index(c, r)];
          if (cell == RoomMask::kOutside &&
              point_in_polygon(polygon, MapPoint{(c + 0.5) / cols, (r + 0.5) / rows})) {
            cell = room;
          }
        }
      }
      continue;
    }
    for (std::int32_t r = 0; r < rows; ++r) {
      const double y = (r + 0.5) / rows;
      std::size_t m = 0;
      for (std::size_t k = 0, j = polygon.size() - 1; k < polygon.size(); j = k++) {
        if (!detail::spans(polygon[j], polygon[k], y)) continue;
        t[m++] = cells_left_of(
            detail::crossing_x(detail::canonical_edge(polygon[j], polygon[k]), y), cols);
      }
      if (m > 0) fill_row(t.data(), m, cols, room, &out.room_index[out.index(0, r)]);
    }
  }
}

RoomMask build_room_mask(const Config& config) {
  RoomMask mask;
  build_room_mask(config, mask);
  return mask;
}

bool begin_heat_field(const RoomMask& mask, HeatField& out) {
  const std::size_t cell_count =
      mask.cols > 0 && mask.rows > 0
          ? static_cast<std::size_t>(mask.cols) * static_cast<std::size_t>(mask.rows)
          : 0;
  if (cell_count == 0 || mask.room_index.size() != cell_count) {  // not a mask: refuse
    out.cols = 0;
    out.rows = 0;
    out.cells.clear();
    return false;
  }
  out.cols = mask.cols;
  out.rows = mask.rows;
  out.cells.assign(cell_count, HeatCell{});
  return true;
}

void compute_heat_field_rows(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples, std::int32_t row_begin,
                             std::int32_t row_end, HeatField& out) {
  if (out.cols != mask.cols || out.rows != mask.rows ||
      out.cells.size() != mask.room_index.size() || out.cells.empty()) {
    return;  // begin_heat_field() refused this mask, or was not called
  }
  const std::size_t room_count = config.rooms.size();
  const auto width = static_cast<float>(config.canvas.map_area.width);
  const auto height = static_cast<float>(config.canvas.map_area.height);
  const auto cols = static_cast<float>(mask.cols);
  const auto rows = static_cast<float>(mask.rows);
  const InverseDistance idw(config.heat.idw_power);

  const std::int32_t first = row_begin < 0 ? 0 : row_begin;
  const std::int32_t last = row_end > mask.rows ? mask.rows : row_end;
  for (std::int32_t r = first; r < last; ++r) {
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
      // The weighted mean is accumulated relative to the room's first usable sample and in a
      // scaled space: base + sum(w * (v - base)) / sum(w), with every value multiplied by
      // kScale = 2^-20 first.
      // - Relative to base: a single sample gives its value exactly, not w * v / w.
      // - Scaled: v - base cannot overflow for finite samples (|v*kScale - base*kScale| is far
      //   below FLT_MAX, and w <= 2^8 since d >= 0.5 px at power <= 8), so the sum stays
      //   finite for fewer than 2^11 samples in a room (a config holds at most 256 pins).
      //   Multiplying by a power of two is exact for normal floats, so for ordinary values the
      //   result is bit-identical to the unscaled sum.
      // A cell whose result still is not finite (every weight underflowed to 0 on an absurd
      // map area, giving 0/0) is NoData, never a garbage Value.
      float base = 0.0F;
      float base_scaled = 0.0F;
      float weighted = 0.0F;
      float total = 0.0F;
      int contributors = 0;
      bool any = false;
      bool coincident = false;
      for (const HeatSample& s : samples) {
        if (!usable(s, room)) continue;
        if (!any) {
          base = s.value;
          base_scaled = s.value * kScale;
        }
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
        weighted += w * (s.value * kScale - base_scaled);
        total += w;
        ++contributors;
      }
      if (!any) {
        cell.state = CellState::NoData;
        continue;
      }
      if (coincident) {
        cell.state = CellState::Value;
        continue;
      }
      // One contributor: its value exactly (base * kScale / kScale would lose a subnormal's
      // low bits).
      const float value = contributors == 1 ? base : (base_scaled + weighted / total) * kUnscale;
      if (!std::isfinite(value)) {
        cell.state = CellState::NoData;
        continue;
      }
      cell.state = CellState::Value;
      cell.value = value;
    }
  }
}

void compute_heat_field(const Config& config, const RoomMask& mask,
                        const std::vector<HeatSample>& samples, HeatField& out) {
  if (!begin_heat_field(mask, out)) return;
  compute_heat_field_rows(config, mask, samples, 0, mask.rows, out);
}

HeatField compute_heat_field(const Config& config, const RoomMask& mask,
                             const std::vector<HeatSample>& samples) {
  HeatField field;
  compute_heat_field(config, mask, samples, field);
  return field;
}

}  // namespace hearthmap
