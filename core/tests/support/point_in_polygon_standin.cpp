// TEMPORARY TEST-ONLY STAND-IN. Delete this file once core's own point_in_polygon()
// (geometry.h, implemented in core/src/geometry.cpp by PR 3) is on the branch.
//
// core/tests/CMakeLists.txt compiles it only while core/src/geometry.cpp does not exist, so the
// suite switches to the real implementation automatically when PR 3 lands; a duplicate-symbol
// link error is the loud failure if the real one arrives under another file name.
//
// It follows geometry.h's stated contract (half-open crossing-number test) so that tests which
// depend on it keep meaning the same thing against the real implementation.
#include <cstddef>

#include "hearthmap/geometry.h"

namespace hearthmap {

bool point_in_polygon(const Polygon& polygon, MapPoint p) {
  bool inside = false;
  const std::size_t n = polygon.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const MapPoint a = polygon[j];
    const MapPoint b = polygon[i];
    if ((a.y > p.y) != (b.y > p.y)) {
      const double x_at = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (p.x < x_at) inside = !inside;
    }
  }
  return inside;
}

}  // namespace hearthmap
