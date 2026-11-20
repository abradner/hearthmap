// config.h: validate(). One test per error and warning the header lists, each starting from a
// clean config (the flat example) and breaking exactly one thing.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/config.h"

using namespace hearthmap;
using namespace hearthmap::test;

namespace {

int count(const std::vector<ConfigIssue>& issues, IssueSeverity sev, const std::string& path) {
  int n = 0;
  for (const auto& i : issues) {
    if (i.severity == sev && i.path == path) ++n;
  }
  return n;
}

bool has_error(const std::vector<ConfigIssue>& issues, const std::string& path) {
  return count(issues, IssueSeverity::Error, path) > 0;
}

bool has_warning(const std::vector<ConfigIssue>& issues, const std::string& path) {
  return count(issues, IssueSeverity::Warning, path) > 0;
}

std::string dump(const std::vector<ConfigIssue>& issues) {
  std::string s;
  for (const auto& i : issues) {
    s += (i.severity == IssueSeverity::Error ? "E " : "W ") + i.path + ": " + i.message + "\n";
  }
  return s;
}

// Exactly one issue, an error at `path`: the mutation broke one thing and was reported once.
void check_only_error(const Config& c, const std::string& path) {
  const auto issues = validate(c);
  INFO(dump(issues));
  CHECK(issues.size() == 1);
  CHECK(has_error(issues, path));
  CHECK_FALSE(usable(issues));
}

std::size_t pi(const Config& c, const char* id) {
  return pin_index(c, id);
}

}  // namespace

TEST_CASE("the flat example validates with no issues") {
  const auto issues = validate(flat_config());
  INFO(dump(issues));
  CHECK(issues.empty());
}

TEST_CASE("duplicate room and pin ids") {
  Config c = flat_config();
  c.rooms[2].id = c.rooms[0].id;
  // The pins in the renamed room now name a room id that still exists (the first), so only
  // the duplicate is reported; the pins' position warnings are about the other room's polygon.
  const auto issues = validate(c);
  INFO(dump(issues));
  CHECK(count(issues, IssueSeverity::Error, "/rooms/2/id") == 1);
  CHECK(count(issues, IssueSeverity::Error, "/rooms/0/id") == 0);  // the first keeps its id

  Config p = flat_config();
  p.pins[5].id = p.pins[1].id;
  check_only_error(p, "/pins/5/id");
}

TEST_CASE("a pin naming an unknown room") {
  Config c = flat_config();
  c.pins[3].room_id = "attic";
  check_only_error(c, "/pins/3/room");
}

TEST_CASE("polygons: fewer than three vertices") {
  Config c = flat_config();
  c.rooms[1].polygon.resize(2);
  // The kitchen's pins can no longer be position-checked; only the polygon is an error.
  check_only_error(c, "/rooms/1/polygon");
  c.rooms[1].polygon.clear();
  check_only_error(c, "/rooms/1/polygon");
}

TEST_CASE("polygons: self-intersecting, overlapping, repeated and degenerate") {
  auto bad = [](Polygon poly) {
    Config c = flat_config();
    c.rooms.push_back(Room{"spare", "Spare", std::move(poly), false});
    return validate(c);
  };
  const std::string path = "/rooms/7/polygon";
  // Bow tie: edges (0,1) and (2,3) cross.
  CHECK(has_error(bad({{0, 0}, {1, 1}, {1, 0}, {0, 1}}), path));
  // A non-adjacent edge touching a vertex (pinched).
  CHECK(has_error(bad({{0, 0}, {1, 0}, {0.5, 0.5}, {1, 1}, {0, 1}, {0.5, 0}}), path));
  // Repeating the first vertex at the end (the schema says not to).
  CHECK(has_error(bad({{0, 0}, {1, 0}, {1, 1}, {0, 0}}), path));
  // A repeated vertex elsewhere.
  CHECK(has_error(bad({{0, 0}, {1, 0}, {1, 0}, {1, 1}}), path));
  // Collinear triangle (zero area): consecutive edges fold back.
  CHECK(has_error(bad({{0, 0}, {1, 0}, {0.5, 0}}), path));
  // A spike: an edge doubles back over the previous one.
  CHECK(has_error(bad({{0, 0}, {1, 0}, {0.5, 0}, {0.5, 1}}), path));
  // Non-finite vertex: reported as a vertex out of range.
  CHECK(
      has_error(bad({{0, 0}, {1, std::numeric_limits<double>::quiet_NaN()}, {1, 1}}), path + "/1"));

  // Positive controls: these are simple.
  CHECK_FALSE(has_error(bad({{0, 0}, {1, 0}, {0, 1}}), path));
  CHECK_FALSE(has_error(bad({{0, 0}, {0.5, 0}, {1, 0}, {1, 1}}), path));  // straight-through vertex
  CHECK_FALSE(has_error(bad({{0, 0}, {1, 0}, {1, 1}, {0.5, 0.5}, {0, 1}}), path));  // concave
  // Winding order does not matter.
  CHECK_FALSE(has_error(bad({{0, 1}, {1, 1}, {1, 0}, {0, 0}}), path));
}

TEST_CASE("pins: missing primary role, a role bound twice") {
  Config c = flat_config();
  c.pins[pi(c, "air_living")].entities = {{Role::Pm25, "sensor.example_living_pm25"}};
  check_only_error(c, "/pins/7/entities");

  Config d = flat_config();
  d.pins[0].entities.push_back({Role::Humidity, "sensor.example_other_humidity"});
  check_only_error(d, "/pins/0/entities");

  // Both at once on one pin are two errors at the same path.
  Config e = flat_config();
  e.pins[1].entities = {{Role::Battery, "sensor.example_a"}, {Role::Battery, "sensor.example_b"}};
  const auto issues = validate(e);
  CHECK(count(issues, IssueSeverity::Error, "/pins/1/entities") == 2);
}

TEST_CASE("pins: calibration required for soil and plant, forbidden otherwise") {
  Config c = flat_config();
  c.pins[pi(c, "plant_basil")].calibration.reset();
  check_only_error(c, "/pins/12/calibration");

  Config s = flat_config();
  s.pins[pi(s, "soil_planter")].calibration.reset();
  check_only_error(s, "/pins/9/calibration");

  Config t = flat_config();
  t.pins[0].calibration = Calibration{};
  check_only_error(t, "/pins/0/calibration");
}

TEST_CASE("calibration: zero width") {
  Config c = flat_config();
  auto& cal = *c.pins[pi(c, "plant_basil")].calibration;
  cal.dry = 20;
  cal.wet = 20;
  check_only_error(c, "/pins/12/calibration");
  cal.wet = 20 + 0.5e-9;  // under the 1e-9 threshold
  check_only_error(c, "/pins/12/calibration");
  cal.wet = 20 + 1e-6;  // usable
  CHECK(validate(c).empty());
  cal.wet = std::numeric_limits<double>::quiet_NaN();
  check_only_error(c, "/pins/12/calibration");
}

TEST_CASE("calibration: happy range within dry..wet and ordered") {
  const std::size_t fiddle = 11;  // dry 10, wet 70, happy 25..60
  Config c = flat_config();
  REQUIRE(c.pins[fiddle].id == "plant_fiddle");

  c.pins[fiddle].calibration->happy_min = 5;  // drier than dry
  check_only_error(c, "/pins/11/calibration/happy_min");
  c.pins[fiddle].calibration->happy_min = 10;  // exactly dry: allowed
  CHECK(validate(c).empty());

  c = flat_config();
  c.pins[fiddle].calibration->happy_max = 71;  // wetter than wet
  check_only_error(c, "/pins/11/calibration/happy_max");
  c.pins[fiddle].calibration->happy_max = 70;  // exactly wet: allowed
  CHECK(validate(c).empty());

  c = flat_config();
  c.pins[fiddle].calibration->happy_min = 61;  // wetter than happy_max 60
  check_only_error(c, "/pins/11/calibration/happy_max");
  c.pins[fiddle].calibration->happy_min = 60;  // equal: allowed
  CHECK(validate(c).empty());

  // Inverted range: happy_min is numerically larger, and that is correct.
  Config inv = flat_config();
  auto& pothos = *inv.pins[pi(inv, "plant_pothos")].calibration;  // dry 2900, wet 1300
  pothos.happy_min = 2500;
  pothos.happy_max = 1700;
  CHECK(validate(inv).empty());
  pothos.happy_min = 1600;  // now wetter than happy_max
  check_only_error(inv, "/pins/13/calibration/happy_max");
  pothos.happy_min = 3000;  // drier than dry on an inverted range
  check_only_error(inv, "/pins/13/calibration/happy_min");
}

TEST_CASE("bands: every ordering step and scale_min < scale_max") {
  auto broken = [](Layer l, auto mutate) {
    Config c = flat_config();
    mutate(c.layers[index_of(l)].band);
    return c;
  };
  check_only_error(broken(Layer::Temperature, [](Band& b) { b.danger_below = 9; }),
                   "/layers/temperature");  // below scale_min 10
  check_only_error(broken(Layer::Temperature, [](Band& b) { b.warn_below = 11; }),
                   "/layers/temperature");  // below danger_below 12
  check_only_error(broken(Layer::Temperature, [](Band& b) { b.comfort_min = 15; }),
                   "/layers/temperature");  // below warn_below 16
  check_only_error(broken(Layer::Humidity, [](Band& b) { b.comfort_max = 39; }),
                   "/layers/humidity");  // below comfort_min
  check_only_error(broken(Layer::Humidity, [](Band& b) { b.warn_above = 59; }), "/layers/humidity");
  check_only_error(broken(Layer::Air, [](Band& b) { b.danger_above = 999; }),
                   "/layers/air");  // below warn_above 1000
  check_only_error(broken(Layer::Air, [](Band& b) { b.scale_max = 1999; }), "/layers/air");
  check_only_error(broken(Layer::Soil,
                          [](Band& b) {
                            b = Band{};
                            b.scale_min = 0.5;
                            b.scale_max = 0.5;
                            b.comfort_min = 0.5;
                            b.comfort_max = 0.5;
                          }),
                   "/layers/soil");  // scale_min == scale_max
  check_only_error(
      broken(Layer::Soil,
             [](Band& b) { b.comfort_min = std::numeric_limits<double>::quiet_NaN(); }),
      "/layers/soil");

  // Equal neighbours are allowed; absent thresholds are skipped; a disabled layer is checked.
  Config ok = flat_config();
  Band& t = ok.layers[index_of(Layer::Temperature)].band;
  t.warn_below = t.comfort_min;
  t.danger_above.reset();
  CHECK(validate(ok).empty());
  Config disabled = broken(Layer::Humidity, [](Band& b) { b.warn_above = 1; });
  disabled.layers[index_of(Layer::Humidity)].enabled = false;
  check_only_error(disabled, "/layers/humidity");
}

TEST_CASE("canvas: orientation and map area") {
  Config c = flat_config();
  c.canvas.width = 800;  // landscape with width == height
  c.canvas.height = 800;
  c.canvas.map_area = CanvasRect{0, 0, 800, 800};
  check_only_error(c, "/canvas/orientation");
  c.canvas.orientation = Orientation::Portrait;  // portrait with height == width
  check_only_error(c, "/canvas/orientation");
  c.canvas.height = 1280;  // portrait, height > width: fine
  CHECK(validate(c).empty());

  auto area = [](CanvasRect r) {
    Config a = flat_config();
    a.canvas.map_area = r;
    return a;
  };
  CHECK(validate(area({0, 0, 1280, 800})).empty());   // the whole canvas
  CHECK(validate(area({320, 0, 960, 800})).empty());  // flush right
  check_only_error(area({321, 0, 960, 800}), "/canvas/map_area");
  check_only_error(area({0, 1, 960, 800}), "/canvas/map_area");
  check_only_error(area({-1, 0, 960, 800}), "/canvas/map_area");
  check_only_error(area({0, -1, 960, 100}), "/canvas/map_area");
  check_only_error(area({0, 0, 0, 800}), "/canvas/map_area");
  check_only_error(area({0, 0, 960, 0}), "/canvas/map_area");
  // x + width overflows int32: must still be caught.
  check_only_error(area({std::numeric_limits<std::int32_t>::max(), 0, 10, 10}), "/canvas/map_area");
}

TEST_CASE("default layer must be enabled") {
  Config c = flat_config();
  c.layers[index_of(Layer::Temperature)].enabled = false;
  check_only_error(c, "/layers/default");
  c.default_layer = Layer::Humidity;
  CHECK(validate(c).empty());
}

TEST_CASE("scalars: stale_after, grid, idw_power, hud") {
  Config c = flat_config();
  c.stale_after = 0;
  check_only_error(c, "/stale_after_s");
  c.stale_after = -5;
  check_only_error(c, "/stale_after_s");
  c.stale_after = 1;
  CHECK(validate(c).empty());

  Config p = flat_config();
  p.pins[0].stale_after = 0;
  check_only_error(p, "/pins/0/stale_after_s");
  p.pins[0].stale_after = 1;
  CHECK(validate(p).empty());

  Config g = flat_config();
  g.heat.grid_cols = 1;
  check_only_error(g, "/heat/grid_cols");
  g.heat.grid_cols = 2;
  g.heat.grid_rows = 1;
  check_only_error(g, "/heat/grid_rows");
  g.heat.grid_rows = 2;
  CHECK(validate(g).empty());

  Config w = flat_config();
  w.heat.idw_power = 0.0;
  check_only_error(w, "/heat/idw_power");
  w.heat.idw_power = std::numeric_limits<double>::quiet_NaN();
  check_only_error(w, "/heat/idw_power");
  w.heat.idw_power = 1e-6;
  CHECK(validate(w).empty());

  Config h = flat_config();
  h.hud.max_items = 0;
  check_only_error(h, "/hud/max_items");
  h.hud.max_items = 1;
  CHECK(validate(h).empty());
}

TEST_CASE("warning: a pin outside its room's polygon; still usable") {
  Config c = flat_config();
  c.pins[1].position = MapPoint{0.1, 0.9};  // kitchen pin moved onto the balcony
  const auto issues = validate(c);
  INFO(dump(issues));
  CHECK(issues.size() == 1);
  CHECK(has_warning(issues, "/pins/1/position"));
  CHECK(usable(issues));
}

TEST_CASE("warning: an enabled layer no pin feeds") {
  Config c = flat_config();
  // Remove every CO2 binding: the air layer has no source.
  std::vector<Pin> kept;
  for (const auto& p : c.pins) {
    if (p.kind != PinKind::AirQuality) kept.push_back(p);
  }
  c.pins = kept;
  auto issues = validate(c);
  INFO(dump(issues));
  CHECK(issues.size() == 1);
  CHECK(has_warning(issues, "/layers/air"));
  CHECK(usable(issues));

  // Disabled: no warning.
  c.layers[index_of(Layer::Air)].enabled = false;
  CHECK(validate(c).empty());

  // Display-only bindings do not count as feeding (D-004): only plants' humidity is left.
  Config h;
  h.rooms.push_back(Room{"r", "R", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, false});
  Pin plant;
  plant.id = "p";
  plant.kind = PinKind::Plant;
  plant.name = "P";
  plant.position = {0.5, 0.5};
  plant.room_id = "r";
  plant.entities = {{Role::Moisture, "sensor.example_m"},
                    {Role::Humidity, "sensor.example_h"},
                    {Role::Temperature, "sensor.example_t"}};
  plant.calibration = Calibration{};
  h.pins.push_back(plant);
  h.default_layer = Layer::Soil;
  issues = validate(h);
  INFO(dump(issues));
  CHECK(has_warning(issues, "/layers/humidity"));
  CHECK(has_warning(issues, "/layers/temperature"));
  CHECK(has_warning(issues, "/layers/air"));
  CHECK_FALSE(has_warning(issues, "/layers/soil"));
}

TEST_CASE("validate reports every problem in one pass") {
  Config c = flat_config();
  c.stale_after = 0;
  c.hud.max_items = 0;
  c.pins[3].room_id = "attic";
  const auto issues = validate(c);
  CHECK(issues.size() == 3);
}

// --- schema ranges and patterns, enforced by validate() too (rulings R1, R3) ----------------

TEST_CASE("rooms: at least one, at most 64") {
  Config c = flat_config();
  c.rooms.clear();
  c.pins.clear();
  auto issues = validate(c);
  CHECK(has_error(issues, "/rooms"));

  Config many = flat_config();
  for (int i = 0; many.rooms.size() < 64; ++i) {
    many.rooms.push_back(
        Room{"extra_" + std::to_string(i), "Extra", {{0, 0}, {0.01, 0}, {0, 0.01}}, false});
  }
  CHECK_FALSE(has_error(validate(many), "/rooms"));  // exactly 64
  many.rooms.push_back(Room{"one_too_many", "Extra", {{0, 0}, {0.01, 0}, {0, 0.01}}, false});
  check_only_error(many, "/rooms");
}

TEST_CASE("pins: at most 256") {
  Config c = flat_config();
  Pin base = c.pins[0];
  base.entities = {{Role::Temperature, "sensor.example_t"}};
  for (int i = 0; c.pins.size() < 256; ++i) {
    base.id = "extra_" + std::to_string(i);
    c.pins.push_back(base);
  }
  CHECK(validate(c).empty());
  base.id = "one_too_many";
  c.pins.push_back(base);
  check_only_error(c, "/pins");
}

TEST_CASE("ids: pattern and length") {
  auto room_id = [](std::string id) {
    Config c = flat_config();
    c.rooms[6].id = id;  // balcony; repoint its pins
    for (auto& p : c.pins) {
      if (p.room_id == "balcony") p.room_id = id;
    }
    return validate(c);
  };
  CHECK(room_id("a").empty());
  CHECK(room_id(std::string(48, 'a')).empty());
  CHECK(has_error(room_id(std::string(49, 'a')), "/rooms/6/id"));
  CHECK(has_error(room_id(""), "/rooms/6/id"));
  CHECK(has_error(room_id("9lives"), "/rooms/6/id"));
  CHECK(has_error(room_id("_x"), "/rooms/6/id"));
  CHECK(has_error(room_id("Balcony"), "/rooms/6/id"));
  CHECK(has_error(room_id("bal-cony"), "/rooms/6/id"));
  CHECK(room_id("b4_l").empty());

  Config p = flat_config();
  p.pins[0].id = "Clim";
  check_only_error(p, "/pins/0/id");
}

TEST_CASE("names: 1..48 Unicode code points of valid UTF-8, as the schema counts them") {
  Config c = flat_config();
  c.rooms[0].name = "";
  check_only_error(c, "/rooms/0/name");
  c.rooms[0].name = std::string(48, 'x');
  CHECK(validate(c).empty());
  c.rooms[0].name = std::string(49, 'x');
  check_only_error(c, "/rooms/0/name");

  SUBCASE("two-byte characters count once each") {
    std::string accented;
    for (int i = 0; i < 25; ++i) accented += "\xC3\xA9";  // 25 x U+00E9: 50 bytes, allowed
    c.rooms[0].name = accented;
    CHECK(validate(c).empty());
    accented.clear();
    for (int i = 0; i < 48; ++i) accented += "\xC3\xA9";  // 48 code points, 96 bytes: allowed
    c.rooms[0].name = accented;
    CHECK(validate(c).empty());
    accented += "\xC3\xA9";  // 49 code points: too long
    c.rooms[0].name = accented;
    check_only_error(c, "/rooms/0/name");
  }

  SUBCASE("three- and four-byte characters count once each") {
    std::string wide;
    for (int i = 0; i < 24; ++i) wide += "\xE2\x82\xAC";      // U+20AC
    for (int i = 0; i < 24; ++i) wide += "\xF0\x9F\x8C\xBF";  // U+1F33F
    c.rooms[0].name = wide;                                   // 48 code points, 168 bytes
    CHECK(validate(c).empty());
    c.rooms[0].name = wide + "x";
    check_only_error(c, "/rooms/0/name");
  }

  SUBCASE("invalid UTF-8 is an error, whatever its length") {
    const char* bad[] = {
        "\x80",              // a lone continuation byte
        "Bad\xC3",           // truncated two-byte sequence
        "\xE2\x82",          // truncated three-byte sequence
        "\xC3\x28",          // a lead byte followed by a non-continuation
        "\xC0\xAF",          // overlong "/"
        "\xE0\x80\xAF",      // overlong "/" in three bytes
        "\xED\xA0\x80",      // U+D800, a surrogate
        "\xF4\x90\x80\x80",  // U+110000, above the Unicode range
        "\xFF",              // never valid
    };
    for (const char* name : bad) {
      INFO("name bytes: " << name);
      c.rooms[0].name = name;
      check_only_error(c, "/rooms/0/name");
    }
    c.rooms[0].name = "\xF4\x8F\xBF\xBF";  // U+10FFFF, the last code point: valid
    CHECK(validate(c).empty());
  }

  SUBCASE("pins follow the same rule") {
    Config p = flat_config();
    p.pins[2].name = "";
    check_only_error(p, "/pins/2/name");
    std::string accented;
    for (int i = 0; i < 48; ++i) accented += "\xC3\xA9";
    p.pins[2].name = accented;
    CHECK(validate(p).empty());
    p.pins[2].name = accented + "\xC3\xA9";
    check_only_error(p, "/pins/2/name");
    p.pins[2].name = "Bad\xC3";
    check_only_error(p, "/pins/2/name");
  }
}

TEST_CASE("positions and vertices within 0..1") {
  Config c = flat_config();
  c.pins[0].position = MapPoint{1.0, 0.0};  // on the edge of map space: in range
  CHECK_FALSE(has_error(validate(c), "/pins/0/position"));
  c.pins[0].position = MapPoint{1.0001, 0.3};
  check_only_error(c, "/pins/0/position");  // no extra outside-room warning
  c.pins[0].position = MapPoint{0.2, -0.0001};
  check_only_error(c, "/pins/0/position");
  c.pins[0].position = MapPoint{std::numeric_limits<double>::quiet_NaN(), 0.3};
  check_only_error(c, "/pins/0/position");

  Config r = flat_config();
  r.rooms[2].polygon[1].x = 1.01;
  const auto issues = validate(r);
  INFO(dump(issues));
  CHECK(has_error(issues, "/rooms/2/polygon/1"));
  CHECK_FALSE(has_error(issues, "/rooms/2/polygon"));  // not also called non-simple
}

TEST_CASE("polygon vertex count at most 64") {
  Polygon poly;
  for (int i = 0; i < 64; ++i) {
    const double a = 6.283185307179586 * i / 64.0;
    poly.push_back({0.5 + 0.4 * std::cos(a), 0.5 + 0.4 * std::sin(a)});
  }
  Config c = flat_config();
  c.rooms.push_back(Room{"round", "Round", poly, false});
  CHECK(validate(c).empty());
  c.rooms.back().polygon.push_back({0.5, 0.5});
  CHECK(has_error(validate(c), "/rooms/7/polygon"));
}

TEST_CASE("entity ids must look like <domain>.<object_id>") {
  for (const char* bad : {"sensor", "sensor.", ".x", "Sensor.x", "sensor.x-y", "sensor x.y"}) {
    CAPTURE(bad);
    Config c = flat_config();
    c.pins[1].entities[0].entity_id = bad;
    check_only_error(c, "/pins/1/entities/temperature");
  }
  Config ok = flat_config();
  ok.pins[1].entities[0].entity_id = "sensor.a.b";  // schema: [a-z0-9_]+\.[a-z0-9_]+ (no 2nd dot)
  check_only_error(ok, "/pins/1/entities/temperature");
  ok.pins[1].entities[0].entity_id = "sensor.k1_t";
  CHECK(validate(ok).empty());
}

TEST_CASE("moisture binds only on soil and plant pins (R1)") {
  Config c = flat_config();
  c.pins[0].entities.push_back({Role::Moisture, "sensor.example_m"});
  check_only_error(c, "/pins/0/entities/moisture");
}

TEST_CASE("photo: plants only, and a valid photo id") {
  Config c = flat_config();
  c.pins[pi(c, "soil_planter")].photo = "planter";
  check_only_error(c, "/pins/9/photo");
  Config t = flat_config();
  t.pins[0].photo = "living";
  check_only_error(t, "/pins/0/photo");
  Config p = flat_config();
  p.pins[pi(p, "plant_basil")].photo = "Basil";
  check_only_error(p, "/pins/12/photo");
  p.pins[pi(p, "plant_basil")].photo = "9-basil_2";
  CHECK(validate(p).empty());
}

TEST_CASE("upper bounds: canvas, grid, idw_power, max_items") {
  Config c = flat_config();
  c.canvas.width = 4096;
  CHECK(validate(c).empty());
  c.canvas.width = 4097;
  check_only_error(c, "/canvas/width");
  Config h = flat_config();
  h.canvas.height = 0;
  h.canvas.map_area = CanvasRect{0, 0, 1, 1};
  const auto hi = validate(h);
  CHECK(has_error(hi, "/canvas/height"));

  Config g = flat_config();
  g.heat.grid_cols = 256;
  g.heat.grid_rows = 256;
  CHECK(validate(g).empty());
  g.heat.grid_cols = 257;
  check_only_error(g, "/heat/grid_cols");
  g.heat.grid_cols = 256;
  g.heat.grid_rows = 257;
  check_only_error(g, "/heat/grid_rows");

  Config w = flat_config();
  w.heat.idw_power = 8.0;
  CHECK(validate(w).empty());
  w.heat.idw_power = 8.0001;
  check_only_error(w, "/heat/idw_power");

  Config m = flat_config();
  m.hud.max_items = 20;
  CHECK(validate(m).empty());
  m.hud.max_items = 21;
  check_only_error(m, "/hud/max_items");
}

TEST_CASE("version must be the schema version") {
  Config c = flat_config();
  c.version = 2;
  check_only_error(c, "/version");
  c.version = 0;
  check_only_error(c, "/version");
}

TEST_CASE("non-finite band values are an error of their own") {
  Config c = flat_config();
  c.layers[index_of(Layer::Humidity)].band.danger_above = std::numeric_limits<double>::infinity();
  c.layers[index_of(Layer::Humidity)].band.scale_max = std::numeric_limits<double>::infinity();
  const auto issues = validate(c);
  INFO(dump(issues));
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].path == "/layers/humidity");
  CHECK(issues[0].message.find("finite") != std::string::npos);
  Config s = flat_config();
  s.layers[index_of(Layer::Soil)].band.scale_min = -std::numeric_limits<double>::infinity();
  check_only_error(s, "/layers/soil");
}

TEST_CASE("Fahrenheit needs every temperature band field set explicitly (D-011)") {
  Config c = flat_config();
  c.temperature_unit = TemperatureUnit::Fahrenheit;
  LayerConfig& t = c.layers[index_of(Layer::Temperature)];
  t.band = Band{66, 75, 61, 81, 54, 90, 50, 93};
  t.present = kAllBandFields;
  CHECK(validate(c).empty());
  // One field left to its (Celsius) default: one error, however many are missing.
  t.present = static_cast<std::uint8_t>(kAllBandFields & ~band_field_bit(BandField::ScaleMax));
  check_only_error(c, "/layers/temperature");
  t.present = 0;
  check_only_error(c, "/layers/temperature");
  // A field set to null counts as present: the source records presence, validate() judges.
  t.present = kAllBandFields;
  t.band.danger_below.reset();
  CHECK(validate(c).empty());
  // Celsius configs never need presence.
  Config celsius = flat_config();
  celsius.layers[index_of(Layer::Temperature)].present = 0;
  CHECK(validate(celsius).empty());
  // Presence on other layers is irrelevant to the rule.
  Config other = c;
  other.layers[index_of(Layer::Humidity)].present = 0;
  CHECK(validate(other).empty());
}
