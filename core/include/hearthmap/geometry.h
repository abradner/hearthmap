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

/// True when `p` lies inside the map area. Edges: left/top inclusive, right/bottom exclusive.
inline bool contains(const CanvasRect& r, CanvasPoint p) {
  return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

/// Even-odd point-in-polygon test in map space.
///
/// Boundary rule (so adjacent rooms never both claim a point): a point exactly on an edge is
/// inside for left and top edges and outside for right and bottom edges, in the sense of the
/// standard half-open crossing-number test (an edge (a, b) counts a crossing when
/// (a.y > p.y) != (b.y > p.y) and p.x < the edge's x at p.y). Implemented in core (PR 3).
bool point_in_polygon(const Polygon& polygon, MapPoint p);

}  // namespace hearthmap
