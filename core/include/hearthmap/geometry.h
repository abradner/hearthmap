// geometry.h — the two coordinate spaces hearthmap uses, and the conversions between them.
//
// Map space (MapPoint): normalised floor-plan coordinates. x runs 0..1 left to right, y runs
// 0..1 top to bottom, origin at the top-left corner of the map area. Rooms and pins are stored
// in this space (schema/README.md, "Coordinates"), so a config survives a change of panel or
// layout.
//
// Canvas space (CanvasPoint): physical panel pixels after rotation, origin top-left. The map
// occupies Canvas::map_area; everything else on the panel (HUD, tabs) is LVGL's business.
//
// Distances that matter for interpolation and hit-testing are measured in canvas pixels, not in
// map units, because map space is stretched by the map area's aspect ratio.
#pragma once

#include <cstdint>
#include <vector>

namespace hearthmap {

/// A point in normalised map space. Values outside 0..1 are legal in arithmetic but never
/// stored in a valid config.
struct MapPoint {
  double x = 0.0;
  double y = 0.0;
};

/// A map-space point in single precision, for the per-frame heat-field path (docs/decisions.md
/// D-022: the device has a single-precision FPU only). Configs store MapPoint; heat samples
/// carry MapPointF.
struct MapPointF {
  float x = 0.0F;
  float y = 0.0F;
};

/// MapPoint -> MapPointF, rounding to the nearest float.
constexpr MapPointF to_float(MapPoint p) {
  return MapPointF{static_cast<float>(p.x), static_cast<float>(p.y)};
}

/// A point in canvas pixels. Floating point so sub-pixel touch positions and cell centres are
/// representable; the UI layer rounds when it draws.
struct CanvasPoint {
  double x = 0.0;
  double y = 0.0;
};

/// An axis-aligned rectangle in canvas pixels. `width` and `height` are > 0 in a valid config.
struct CanvasRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

/// A simple polygon in map space: at least three vertices, implicitly closed (the last vertex
/// connects back to the first; do not repeat the first vertex). Winding order is irrelevant.
/// Self-intersecting polygons are rejected by config validation.
using Polygon = std::vector<MapPoint>;

/// Map space -> canvas pixels for a given map area.
inline CanvasPoint to_canvas(const CanvasRect& map_area, MapPoint p) {
  return CanvasPoint{map_area.x + p.x * map_area.width, map_area.y + p.y * map_area.height};
}

/// Canvas pixels -> map space for a given map area. The caller guarantees a non-degenerate
/// map area (validated config); points outside the area map outside 0..1.
inline MapPoint to_map(const CanvasRect& map_area, CanvasPoint p) {
  return MapPoint{(p.x - map_area.x) / map_area.width, (p.y - map_area.y) / map_area.height};
}

/// `p` turned clockwise about the centre of map space by `quarter_turns` x 90 degrees, as seen
/// on the panel (y down): one quarter turn takes (x, y) to (1 - y, x), two to (1 - x, 1 - y),
/// three to (y, 1 - x). Normalised map space needs no aspect ratio for this: the map area the
/// result is drawn into carries the turned proportions (canvas.map_area, schema/README.md).
/// `quarter_turns` is taken modulo 4; negative turns go anticlockwise. Exact for 0 turns and
/// whenever 1 - v is exact (every v >= 0.5, and dyadic values); otherwise each turn rounds
/// 1 - v once, to within half an ulp of 1, so four turns come back to within 4e-16 of `p`.
MapPoint rotate_map_point(MapPoint p, int quarter_turns);

/// True when `p` lies inside the map area. Edges: left/top inclusive, right/bottom exclusive.
inline bool contains(const CanvasRect& r, CanvasPoint p) {
  return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

/// Even-odd point-in-polygon test in map space.
///
/// Boundary rule (so adjacent rooms never both claim a point): a point exactly on an edge is
/// inside for left and top edges and outside for right and bottom edges, in the sense of the
/// standard half-open crossing-number test (an edge (a, b) counts a crossing when
/// (a.y > p.y) != (b.y > p.y) and p.x < the edge's x at p.y). Each edge is evaluated in a
/// canonical vertex order, so two polygons that share an edge (the same two vertices, walked in
/// either direction) compute bit-identical crossings for it: a point on a shared diagonal wall
/// goes to exactly one of them. (An edge split at different vertices in the two polygons gets no
/// such guarantee.) Fewer than three vertices, or zero area: never inside. Zero area is
/// detected explicitly before crossings are counted (collinear edges can round to an odd
/// count): all vertices on one line, within a tolerance of a few ulps of the coordinates'
/// magnitude, so collinear input rounded to doubles still counts as collinear while a real
/// sliver (1e-6 thick in map space) does not.
bool point_in_polygon(const Polygon& polygon, MapPoint p);

}  // namespace hearthmap
