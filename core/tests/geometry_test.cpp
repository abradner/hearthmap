// Tests for point_in_polygon() (geometry.h): even-odd rule, the half-open edge rule, concave
// and degenerate polygons, and shared-edge exclusivity.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "hearthmap/geometry.h"

using namespace hearthmap;

namespace {

const Polygon kUnitSquare{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};

}  // namespace

TEST_CASE("point_in_polygon: interior and exterior of a square") {
  CHECK(point_in_polygon(kUnitSquare, {0.5, 0.5}));
  CHECK(point_in_polygon(kUnitSquare, {0.001, 0.999}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {1.5, 0.5}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {-0.5, 0.5}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.5, -0.5}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.5, 1.5}));
}

TEST_CASE("point_in_polygon: left and top edges are inside, right and bottom outside") {
  CHECK(point_in_polygon(kUnitSquare, {0.0, 0.5}));        // left edge
  CHECK(point_in_polygon(kUnitSquare, {0.5, 0.0}));        // top edge
  CHECK_FALSE(point_in_polygon(kUnitSquare, {1.0, 0.5}));  // right edge
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.5, 1.0}));  // bottom edge
  CHECK(point_in_polygon(kUnitSquare, {0.0, 0.0}));        // top-left corner
  CHECK_FALSE(point_in_polygon(kUnitSquare, {1.0, 0.0}));  // top-right corner
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.0, 1.0}));  // bottom-left corner
  CHECK_FALSE(point_in_polygon(kUnitSquare, {1.0, 1.0}));  // bottom-right corner
}

TEST_CASE("point_in_polygon: winding order does not matter") {
  const Polygon clockwise{{0.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}};
  CHECK(point_in_polygon(clockwise, {0.5, 0.5}));
  CHECK(point_in_polygon(clockwise, {0.0, 0.5}));
  CHECK_FALSE(point_in_polygon(clockwise, {1.0, 0.5}));
  CHECK_FALSE(point_in_polygon(clockwise, {0.5, 1.0}));
}

TEST_CASE("point_in_polygon: adjacent rooms never both claim a point on their shared wall") {
  const Polygon left{{0.0, 0.0}, {0.5, 0.0}, {0.5, 1.0}, {0.0, 1.0}};
  const Polygon right{{0.5, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.5, 1.0}};
  const Polygon below{{0.0, 1.0}, {1.0, 1.0}, {1.0, 2.0}, {0.0, 2.0}};
  // Vertical wall: the room to the right owns it.
  CHECK_FALSE(point_in_polygon(left, {0.5, 0.25}));
  CHECK(point_in_polygon(right, {0.5, 0.25}));
  // Horizontal wall: the room below owns it.
  CHECK_FALSE(point_in_polygon(left, {0.25, 1.0}));
  CHECK(point_in_polygon(below, {0.25, 1.0}));
}

TEST_CASE("point_in_polygon: a shared diagonal wall goes to exactly one room") {
  // Two triangles splitting a square along an awkward diagonal; the two walk the shared edge in
  // opposite directions. Points are sampled on the edge itself (computed the way a caller
  // would, so not exactly representable) and just either side of it.
  const MapPoint a{0.1, 0.03};
  const MapPoint b{0.93, 0.71};
  const Polygon upper{a, {0.93, 0.03}, b};
  const Polygon lower{a, b, {0.1, 0.71}};
  int both = 0;
  int neither = 0;
  for (int i = 1; i < 1000; ++i) {
    const double t = i / 1000.0;
    const MapPoint on{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
    const bool in_upper = point_in_polygon(upper, on);
    const bool in_lower = point_in_polygon(lower, on);
    if (in_upper && in_lower) ++both;
    if (!in_upper && !in_lower) ++neither;
  }
  CHECK(both == 0);
  CHECK(neither == 0);
}

TEST_CASE("point_in_polygon: concave L-shape excludes its notch") {
  // An L: a full-width top bar and a left leg; the notch is the bottom-right quadrant.
  const Polygon l_shape{{0.0, 0.0}, {1.0, 0.0}, {1.0, 0.5}, {0.5, 0.5}, {0.5, 1.0}, {0.0, 1.0}};
  CHECK(point_in_polygon(l_shape, {0.75, 0.25}));        // top bar
  CHECK(point_in_polygon(l_shape, {0.25, 0.75}));        // leg
  CHECK_FALSE(point_in_polygon(l_shape, {0.75, 0.75}));  // notch
  // Edges of the notch follow the same half-open rule as any other edge.
  CHECK_FALSE(point_in_polygon(l_shape, {0.5, 0.75}));  // leg's right edge: outside
  CHECK_FALSE(point_in_polygon(l_shape, {0.75, 0.5}));  // bar's bottom edge: outside
  CHECK(point_in_polygon(l_shape, {0.25, 0.5}));        // level with the inner corner, in the leg
  // The inner (reflex) vertex itself: its edges are the bar's bottom and the leg's right side.
  CHECK_FALSE(point_in_polygon(l_shape, {0.5, 0.5}));
}

TEST_CASE("point_in_polygon: degenerate polygons contain nothing") {
  CHECK_FALSE(point_in_polygon(Polygon{}, {0.0, 0.0}));
  CHECK_FALSE(point_in_polygon(Polygon{{0.5, 0.5}}, {0.5, 0.5}));
  CHECK_FALSE(point_in_polygon(Polygon{{0.0, 0.0}, {1.0, 1.0}}, {0.5, 0.5}));
  // Zero area: collinear vertices, a point on the segment and points beside it.
  const Polygon collinear{{0.0, 0.0}, {0.5, 0.5}, {1.0, 1.0}};
  CHECK_FALSE(point_in_polygon(collinear, {0.25, 0.25}));
  CHECK_FALSE(point_in_polygon(collinear, {0.2, 0.25}));
  CHECK_FALSE(point_in_polygon(collinear, {0.3, 0.25}));
  // Zero area, horizontal: every edge is horizontal and never counts.
  const Polygon flat{{0.0, 0.5}, {1.0, 0.5}, {0.5, 0.5}};
  CHECK_FALSE(point_in_polygon(flat, {0.5, 0.5}));
  // All vertices the same point.
  const Polygon dot{{0.5, 0.5}, {0.5, 0.5}, {0.5, 0.5}};
  CHECK_FALSE(point_in_polygon(dot, {0.5, 0.5}));
}

TEST_CASE("point_in_polygon: a non-finite point is outside") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  CHECK_FALSE(point_in_polygon(kUnitSquare, {nan, 0.5}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.5, nan}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {-inf, 0.5}));
  CHECK_FALSE(point_in_polygon(kUnitSquare, {0.5, inf}));
}

TEST_CASE("point_in_polygon: collinear vertices contain nothing, however the crossings round") {
  // Review finding on PR 3: exactly zero area, yet the overlapping edges' intersections rounded
  // differently and the crossing count came out odd.
  const Polygon collinear{{0.0, 0.0}, {0.3125, 0.4375}, {0.625, 0.875}};
  CHECK_FALSE(point_in_polygon(collinear, {0.35, 0.49}));
  // Collinear in intent but not exactly representable: points on y = 0.7x + 0.05, each rounded
  // to the nearest double, so their cross products are rounding noise rather than zero.
  auto on_line = [](double x) { return MapPoint{x, 0.7 * x + 0.05}; };
  const Polygon inexact{on_line(0.1), on_line(0.3), on_line(0.9), on_line(0.55)};
  // Three points a hair apart, far from the origin: their coordinates' rounding is large next
  // to their spacing.
  const Polygon close_together{on_line(0.9), on_line(0.9 + 1e-9), on_line(0.9 + 3e-9)};
  int inside = 0;
  for (int i = 0; i <= 2000; ++i) {
    const double x = 0.05 + 0.9 * i / 2000.0;
    const MapPoint p = on_line(x);
    inside += point_in_polygon(inexact, p) ? 1 : 0;
    inside += point_in_polygon(inexact, {p.x, std::nextafter(p.y, 1.0)}) ? 1 : 0;
    inside += point_in_polygon(inexact, {p.x, std::nextafter(p.y, 0.0)}) ? 1 : 0;
    const double xc = 0.9 + 3e-9 * i / 2000.0;
    const MapPoint q = on_line(xc);
    inside += point_in_polygon(close_together, q) ? 1 : 0;
    inside += point_in_polygon(close_together, {q.x, std::nextafter(q.y, 1.0)}) ? 1 : 0;
    inside += point_in_polygon(close_together, {q.x, std::nextafter(q.y, 0.0)}) ? 1 : 0;
  }
  CHECK(inside == 0);
}

TEST_CASE("point_in_polygon: a thin but real sliver still contains its interior") {
  const Polygon sliver{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1e-6}};  // area 5e-7
  CHECK(point_in_polygon(sliver, {0.5, 2.5e-7}));
  CHECK(point_in_polygon(sliver, {0.99, 0.5e-6}));
  CHECK_FALSE(point_in_polygon(sliver, {0.5, 6e-7}));
  // A sliver whose vertices are far from its first vertex in one axis only.
  const Polygon needle{{0.2, 0.5}, {0.8, 0.5 + 1e-9}, {0.8, 0.5 - 1e-9}};
  CHECK(point_in_polygon(needle, {0.7, 0.5}));
}
