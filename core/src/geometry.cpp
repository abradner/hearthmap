// geometry.cpp — point-in-polygon with the half-open boundary rule stated in geometry.h.
#include "hearthmap/geometry.h"

#include <cmath>
#include <cstddef>
#include <limits>

#include "polygon_edges.h"

namespace hearthmap {

namespace {

// Does the edge between `u` and `v` cross the horizontal ray going right from `p`?
//
// The edge is canonicalised (lower y first, then lower x) before the intersection is computed,
// so the two rooms that share an edge evaluate exactly the same floating-point expression for
// it, whichever direction each polygon walks it. Without this, a point within one rounding error
// of a shared diagonal wall could be claimed by both rooms or by neither. The arithmetic lives in
// polygon_edges.h, shared with RoomScanner.
bool crosses(MapPoint u, MapPoint v, MapPoint p) {
  if (!detail::spans(u, v, p.y)) return false;  // also excludes horizontal edges
  return p.x < detail::crossing_x(detail::canonical_edge(u, v), p.y);
}

}  // namespace

namespace detail {

// True when every vertex lies on one line (zero area). The line runs from vertex 0 to the vertex
// farthest from it; a vertex is off the line when its cross product with that direction is
// larger than the rounding error the inputs could carry. That bound is absolute, scaled by the
// coordinates' magnitude, not relative to the cross product's own terms: vertices meant to be
// collinear arrive rounded to the nearest double, an error proportional to the coordinates, so
// three points a hair apart near (0.9, 0.9) are still "on the line". A real room, however thin,
// is off the line by far more (a 1e-6 sliver in unit map space is ~10^8 tolerances away).
bool collinear(const Polygon& polygon) {
  const MapPoint origin = polygon[0];
  double far_x = 0.0;
  double far_y = 0.0;
  double far_d2 = 0.0;
  for (const MapPoint& v : polygon) {
    const double dx = v.x - origin.x;
    const double dy = v.y - origin.y;
    const double d2 = dx * dx + dy * dy;
    if (d2 > far_d2) {
      far_d2 = d2;
      far_x = dx;
      far_y = dy;
    }
  }
  if (!(far_d2 > 0.0)) return true;  // every vertex in one place (or NaN coordinates)
  constexpr double kUlps = 16.0 * std::numeric_limits<double>::epsilon();
  const double direction = std::fabs(far_x) + std::fabs(far_y);
  const double origin_size = std::fabs(origin.x) + std::fabs(origin.y);
  for (const MapPoint& v : polygon) {
    const double cross = far_x * (v.y - origin.y) - far_y * (v.x - origin.x);
    const double bound = kUlps * direction * (std::fabs(v.x) + std::fabs(v.y) + origin_size);
    if (std::fabs(cross) > bound) return false;
  }
  return true;
}

}  // namespace detail

// O(n) in the vertex count. Fewer than three vertices encloses nothing, so it is never inside;
// a zero-area polygon (all vertices collinear) is rejected explicitly before counting, because
// its overlapping edges can round to different intersections and leave an odd crossing count.
// NaN coordinates compare false everywhere and come out outside.
// Clockwise on the panel, where y points down: the top-left corner (0, 0) goes to the top-right
// (1, 0). One subtraction per coordinate and turn, so the only rounding is 1 - v's (geometry.h).
MapPoint rotate_map_point(MapPoint p, int quarter_turns) {
  switch (((quarter_turns % 4) + 4) % 4) {
    case 1: return MapPoint{1.0 - p.y, p.x};
    case 2: return MapPoint{1.0 - p.x, 1.0 - p.y};
    case 3: return MapPoint{p.y, 1.0 - p.x};
    default: return p;
  }
}

bool point_in_polygon(const Polygon& polygon, MapPoint p) {
  const std::size_t n = polygon.size();
  if (n < 3 || detail::collinear(polygon)) return false;
  bool inside = false;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    if (crosses(polygon[j], polygon[i], p)) inside = !inside;
  }
  return inside;
}

}  // namespace hearthmap
