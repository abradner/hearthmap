// geometry.cpp — point-in-polygon with the half-open boundary rule stated in geometry.h.
#include "hearthmap/geometry.h"

#include <cstddef>

namespace hearthmap {

namespace {

// Does the edge between `u` and `v` cross the horizontal ray going right from `p`?
//
// The edge is canonicalised (lower y first, then lower x) before the intersection is computed,
// so the two rooms that share an edge evaluate exactly the same floating-point expression for
// it, whichever direction each polygon walks it. Without this, a point within one rounding error
// of a shared diagonal wall could be claimed by both rooms or by neither.
bool crosses(MapPoint u, MapPoint v, MapPoint p) {
  if ((u.y > p.y) == (v.y > p.y)) return false;  // also excludes horizontal edges
  const bool swap = (v.y < u.y) || (v.y == u.y && v.x < u.x);
  const MapPoint a = swap ? v : u;
  const MapPoint b = swap ? u : v;
  const double x_at_p = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
  return p.x < x_at_p;
}

}  // namespace

// O(n) in the vertex count. Fewer than three vertices encloses nothing, so it is never inside;
// a zero-area polygon (all vertices collinear) needs no special case: every ray crosses it an
// even number of times. NaN coordinates compare false everywhere and come out outside.
bool point_in_polygon(const Polygon& polygon, MapPoint p) {
  const std::size_t n = polygon.size();
  if (n < 3) return false;
  bool inside = false;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    if (crosses(polygon[j], polygon[i], p)) inside = !inside;
  }
  return inside;
}

}  // namespace hearthmap
