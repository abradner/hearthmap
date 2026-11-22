// canvas.map_rotation: rotate_map_point() (geometry.h), apply_map_rotation() and validate()'s
// range check (config.h). The rotation is applied once, by the config source, so these tests
// check the turned Config itself and what hit-testing and validation then see in it.
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/config.h"
#include "hearthmap/geometry.h"
#include "hearthmap/view_model.h"

using namespace hearthmap;
using namespace hearthmap::test;

namespace {

bool has_error_at(const std::vector<ConfigIssue>& issues, const std::string& path) {
  for (const auto& i : issues) {
    if (i.severity == IssueSeverity::Error && i.path == path) return true;
  }
  return false;
}

std::size_t error_count(const std::vector<ConfigIssue>& issues) {
  std::size_t n = 0;
  for (const auto& i : issues) n += i.severity == IssueSeverity::Error ? 1U : 0U;
  return n;
}

// A one-room config whose room is the unit square and whose single pin sits near the plan's
// top-left corner, on a non-square map area (300 wide, 200 tall at 100, 50).
Config corner_config(std::int32_t rotation) {
  Config c;
  c.canvas.map_area = CanvasRect{100, 50, 300, 200};
  c.canvas.map_rotation = rotation;
  c.rooms = {Room{"all", "All", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, false}};
  Pin p;
  p.id = "corner";
  p.kind = PinKind::Temperature;
  p.name = "Corner";
  p.position = {0.0, 0.0};
  p.room_id = "all";
  p.entities = {{Role::Temperature, "sensor.example_corner"}};
  c.pins.push_back(p);
  return c;
}

}  // namespace

TEST_CASE("map_rotation defaults to 0 and a config without it is unchanged by applying it") {
  CHECK(Canvas{}.map_rotation == 0);
  CHECK(Config{}.canvas.map_rotation == 0);
  const Config before = flat_config();
  Config after = before;
  apply_map_rotation(after);
  REQUIRE(after.rooms.size() == before.rooms.size());
  for (std::size_t r = 0; r < before.rooms.size(); ++r) {
    for (std::size_t v = 0; v < before.rooms[r].polygon.size(); ++v) {
      CHECK(after.rooms[r].polygon[v].x == before.rooms[r].polygon[v].x);
      CHECK(after.rooms[r].polygon[v].y == before.rooms[r].polygon[v].y);
    }
  }
  for (std::size_t i = 0; i < before.pins.size(); ++i) {
    CHECK(after.pins[i].position.x == before.pins[i].position.x);
    CHECK(after.pins[i].position.y == before.pins[i].position.y);
  }
}

TEST_CASE("rotate_map_point: one quarter turn clockwise as seen on the panel (y down)") {
  // The plan's corners go round clockwise: top-left -> top-right -> bottom-right -> bottom-left.
  const MapPoint tl{0, 0}, tr{1, 0}, br{1, 1}, bl{0, 1};
  auto eq = [](MapPoint a, MapPoint b) { return a.x == b.x && a.y == b.y; };
  CHECK(eq(rotate_map_point(tl, 1), tr));
  CHECK(eq(rotate_map_point(tr, 1), br));
  CHECK(eq(rotate_map_point(br, 1), bl));
  CHECK(eq(rotate_map_point(bl, 1), tl));
  CHECK(eq(rotate_map_point(tl, 2), br));
  CHECK(eq(rotate_map_point(tl, 3), bl));
  CHECK(eq(rotate_map_point({0.25, 0.75}, 0), MapPoint{0.25, 0.75}));
  // Exact values for dyadic inputs: (x, y) -> (1 - y, x), (1 - x, 1 - y), (y, 1 - x).
  CHECK(eq(rotate_map_point({0.25, 0.125}, 1), MapPoint{0.875, 0.25}));
  CHECK(eq(rotate_map_point({0.25, 0.125}, 2), MapPoint{0.75, 0.875}));
  CHECK(eq(rotate_map_point({0.25, 0.125}, 3), MapPoint{0.125, 0.75}));
  // Quarter turns are taken modulo 4; negative turns go anticlockwise.
  CHECK(eq(rotate_map_point({0.25, 0.125}, 5), rotate_map_point({0.25, 0.125}, 1)));
  CHECK(eq(rotate_map_point({0.25, 0.125}, -1), rotate_map_point({0.25, 0.125}, 3)));
}

TEST_CASE("rotate_map_point: four quarter turns come back to the start") {
  // Exact for every point whose 1 - v is exact (dyadic coordinates); otherwise each turn rounds
  // 1 - v once, so the round trip is within a few ulps of 1 (stated in geometry.h: 4e-16).
  const MapPoint exact[] = {{0.0, 0.0}, {0.5, 0.25}, {0.75, 1.0}, {0.125, 0.875}};
  for (MapPoint p : exact) {
    MapPoint q = p;
    for (int i = 0; i < 4; ++i) q = rotate_map_point(q, 1);
    CHECK(q.x == p.x);
    CHECK(q.y == p.y);
  }
  const Config flat = flat_config();
  double worst = 0.0;
  for (const Room& room : flat.rooms) {
    for (MapPoint p : room.polygon) {
      MapPoint q = p;
      for (int i = 0; i < 4; ++i) q = rotate_map_point(q, 1);
      worst = std::fmax(worst, std::fmax(std::fabs(q.x - p.x), std::fabs(q.y - p.y)));
    }
  }
  for (const Pin& pin : flat.pins) {
    MapPoint q = pin.position;
    for (int i = 0; i < 4; ++i) q = rotate_map_point(q, 1);
    worst = std::fmax(worst,
                      std::fmax(std::fabs(q.x - pin.position.x), std::fabs(q.y - pin.position.y)));
  }
  CHECK(worst <= 4e-16);
}

TEST_CASE("apply_map_rotation: all four angles on a non-square map area") {
  // The pin at the plan's top-left corner lands on the map area's top-left, top-right,
  // bottom-right and bottom-left corner in canvas pixels; map_area itself never moves.
  struct Case {
    std::int32_t rotation;
    CanvasPoint expected;
  };
  const Case cases[] = {
      {0, {100, 50}},
      {90, {400, 50}},
      {180, {400, 250}},
      {270, {100, 250}},
  };
  for (const Case& k : cases) {
    CAPTURE(k.rotation);
    Config c = corner_config(k.rotation);
    REQUIRE(usable(validate(c)));
    apply_map_rotation(c);
    CHECK(c.canvas.map_rotation == 0);  // the config is now in the panel's frame
    CHECK(c.canvas.map_area.x == 100);
    CHECK(c.canvas.map_area.width == 300);
    const CanvasPoint at = to_canvas(c.canvas.map_area, c.pins[0].position);
    CHECK(at.x == doctest::Approx(k.expected.x));
    CHECK(at.y == doctest::Approx(k.expected.y));
  }
}

TEST_CASE("apply_map_rotation: applying twice is applying once") {
  Config once = corner_config(90);
  apply_map_rotation(once);
  Config twice = once;
  apply_map_rotation(twice);
  CHECK(twice.pins[0].position.x == once.pins[0].position.x);
  CHECK(twice.pins[0].position.y == once.pins[0].position.y);
}

TEST_CASE("apply_map_rotation: the flat stays valid, rooms simple, pins in their rooms") {
  for (std::int32_t rotation : {0, 90, 180, 270}) {
    CAPTURE(rotation);
    Config c = flat_config();
    c.canvas.map_rotation = rotation;
    REQUIRE(error_count(validate(c)) == 0);
    apply_map_rotation(c);
    // Every polygon is still simple (validate() checks it) and nothing else broke.
    CHECK(error_count(validate(c)) == 0);
    // Room order is kept, so the earlier room still owns overlaps.
    CHECK(c.rooms[0].id == "living");
    // Every pin is still inside the room it names.
    for (const Pin& pin : c.pins) {
      CAPTURE(pin.id);
      const Room& room = c.rooms[room_index(c, pin.room_id)];
      CHECK(point_in_polygon(room.polygon, pin.position));
    }
  }
}

TEST_CASE("hit_test finds a pin where the rotated map draws it, not where the plan had it") {
  // The flat turned a quarter turn clockwise, into a map area with the plan's proportions
  // swapped (864 x 720 becomes 600 x 720). Basil sits at (0.625, 0.06) in the plan, so on the
  // panel it is at (1 - 0.06, 0.625) of the area: canvas (40 + 0.94 * 600, 76 + 0.625 * 720).
  Config c = flat_config();
  c.canvas.map_area = CanvasRect{40, 76, 600, 720};
  c.canvas.map_rotation = 90;
  REQUIRE(error_count(validate(c)) == 0);
  apply_map_rotation(c);
  const std::size_t basil = pin_index(c, "plant_basil");
  const HitResult hit = hit_test(c, CanvasPoint{604.0, 526.0});
  CHECK(hit.kind == HitResult::Kind::Pin);
  CHECK(hit.index == basil);
  // Where the unrotated plan would have drawn it (40 + 0.625 * 600, 76 + 0.06 * 720) is not
  // basil any more.
  const HitResult old = hit_test(c, CanvasPoint{415.0, 119.2});
  CHECK_FALSE((old.kind == HitResult::Kind::Pin && old.index == basil));
  // A room is hit where the rotated map draws it too: the living room is the plan's top-left,
  // so on the panel it is the top-right of the area.
  const HitResult room = hit_test(c, CanvasPoint{40 + 0.99 * 600, 76 + 0.01 * 720});
  CHECK(room.kind == HitResult::Kind::Room);
  CHECK(room.index == room_index(c, "living"));
}

TEST_CASE("validate: map_rotation must be 0, 90, 180 or 270") {
  for (std::int32_t ok : {0, 90, 180, 270}) {
    Config c = corner_config(ok);
    CHECK_FALSE(has_error_at(validate(c), "/canvas/map_rotation"));
  }
  for (std::int32_t bad : {45, -90, 360, 1, 271}) {
    CAPTURE(bad);
    Config c = corner_config(bad);
    CHECK(has_error_at(validate(c), "/canvas/map_rotation"));
  }
}

TEST_CASE("apply_map_rotation leaves a config with an invalid rotation alone") {
  // Sources call it only after validate() accepted the config; on anything else it is a no-op
  // rather than a guess.
  Config c = corner_config(45);
  apply_map_rotation(c);
  CHECK(c.canvas.map_rotation == 45);
  CHECK(c.pins[0].position.x == 0.0);
  CHECK(c.pins[0].position.y == 0.0);
}
