// polygon_edges.h — private to core: the one definition of how a polygon edge meets a
// horizontal line. point_in_polygon() (geometry.cpp) and RoomScanner (heat_field.cpp) both use
// it, so a scanline answer is the per-point answer bit for bit, not an approximation of it.
#pragma once

#include "hearthmap/geometry.h"

namespace hearthmap::detail {

/// An edge canonicalised (lower y first, then lower x) so the two rooms sharing a wall evaluate
/// exactly the same expression for it, whichever direction each polygon walks it.
struct CanonicalEdge {
  MapPoint a;
  double dx = 0.0;  ///< b.x - a.x
  double dy = 0.0;  ///< b.y - a.y
  double y_lo = 0.0;
  double y_hi = 0.0;
};

inline CanonicalEdge canonical_edge(MapPoint u, MapPoint v) {
  const bool swap = (v.y < u.y) || (v.y == u.y && v.x < u.x);
  const MapPoint a = swap ? v : u;
  const MapPoint b = swap ? u : v;
  return CanonicalEdge{a, b.x - a.x, b.y - a.y, a.y, b.y};
}

/// Does the edge (u, v) cross the horizontal line at `y` under the half-open rule? Excludes
/// horizontal edges. Depends only on the endpoints' y, so it may be tested on either form.
inline bool spans(MapPoint u, MapPoint v, double y) {
  return (u.y > y) != (v.y > y);
}

/// The x where a spanning edge meets the line at `y`. The expression and its evaluation order
/// are the contract: a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y), with core built with
/// -ffp-contract=off (D-024).
inline double crossing_x(const CanonicalEdge& e, double y) {
  return e.a.x + (y - e.a.y) * e.dx / e.dy;
}

/// True when every vertex lies on one line (zero area): such a polygon encloses nothing.
bool collinear(const Polygon& polygon);

}  // namespace hearthmap::detail
