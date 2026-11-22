#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "json_config.h"

using namespace hearthmap;
using namespace hearthmap::host;

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string example() {
  return slurp(std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/config.json");
}

// A small valid config; `extra` is spliced in as additional top-level members.
std::string tiny(const std::string& extra = "", const std::string& canvas_extra = "") {
  return R"({"version": 1,
    "canvas": {"width": 1280, "height": 800, "orientation": "landscape")" +
         canvas_extra + R"(},
    "rooms": [{"id": "a", "name": "A", "polygon": [[0,0],[1,0],[1,1],[0,1]]}],
    "pins": [{"id": "t", "kind": "temperature", "name": "T", "position": [0.5, 0.5],
              "room": "a", "entities": {"temperature": "sensor.example_t"}}])" +
         extra + "}";
}

bool has_issue(const ConfigLoadResult& r, const std::string& path, IssueSeverity sev) {
  for (const auto& i : r.issues) {
    if (i.path == path && i.severity == sev) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("the example flat loads, with its explicit map area and partial air override") {
  const ConfigLoadResult r = JsonConfigSource(example()).load();
  REQUIRE(r.config.has_value());
  const Config& c = *r.config;
  CHECK(c.canvas.map_area.x == 40);
  CHECK(c.canvas.map_area.y == 76);
  CHECK(c.canvas.map_area.width == 864);
  CHECK(c.canvas.map_area.height == 720);
  CHECK(c.rooms.size() == 7);
  CHECK(c.pins.size() == 18);
  CHECK(c.hud.max_items == 4);
  CHECK(c.rooms[6].outdoor);
  const LayerConfig& air = c.layer(Layer::Air);
  REQUIRE(air.band.warn_above.has_value());
  CHECK(*air.band.warn_above == doctest::Approx(1000));
  CHECK(air.present == band_field_bit(BandField::WarnAbove));
  CHECK(c.layer(Layer::Temperature).present == 0);
  // A pin's optional parts.
  CHECK(c.pins[12].photo.value_or("") == "basil");
  CHECK(c.pins[17].calibration.has_value());
  CHECK(c.pins[16].stale_after.value_or(0) == 21600);
}

TEST_CASE("an omitted map area is the whole canvas") {
  const ConfigLoadResult r = JsonConfigSource(tiny()).load();
  REQUIRE(r.config.has_value());
  CHECK(r.config->canvas.map_area.width == 1280);
  CHECK(r.config->canvas.map_area.height == 800);
  CHECK(r.config->canvas.map_area.x == 0);
  CHECK(r.config->hud.max_items == 6);    // schema default
  CHECK(r.config->stale_after == 10800);  // schema default
  CHECK(r.config->heat.grid_cols == 48);
}

TEST_CASE("a null threshold removes it and still counts as set") {
  const ConfigLoadResult r =
      JsonConfigSource(
          tiny(R"(, "layers": {"humidity": {"warn_below": null, "danger_below": null}})"))
          .load();
  REQUIRE(r.config.has_value());
  const LayerConfig& h = r.config->layer(Layer::Humidity);
  CHECK_FALSE(h.band.warn_below.has_value());
  CHECK_FALSE(h.band.danger_below.has_value());
  CHECK(h.band.warn_above.has_value());
  CHECK(h.present ==
        (band_field_bit(BandField::WarnBelow) | band_field_bit(BandField::DangerBelow)));
}

TEST_CASE("parser-only errors carry their JSON path") {
  SUBCASE("malformed JSON") {
    const ConfigLoadResult r = JsonConfigSource("{ not json").load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/", IssueSeverity::Error));
  }
  SUBCASE("unknown field") {
    const ConfigLoadResult r = JsonConfigSource(tiny(R"(, "colour": "blue")")).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/colour", IssueSeverity::Error));
  }
  SUBCASE("wrong type") {
    const ConfigLoadResult r = JsonConfigSource(tiny(R"(, "stale_after_s": "soon")")).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/stale_after_s", IssueSeverity::Error));
  }
  SUBCASE("unknown role") {
    std::string t = tiny();
    t.replace(t.find("\"temperature\": \"sensor"), 13, "\"warmth\"");
    const ConfigLoadResult r = JsonConfigSource(t).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/pins/0/entities/warmth", IssueSeverity::Error));
  }
  SUBCASE("missing required field") {
    std::string t = tiny();
    t.replace(t.find("\"version\": 1,"), 13, "");
    const ConfigLoadResult r = JsonConfigSource(t).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/version", IssueSeverity::Error));
  }
}

TEST_CASE("semantic rules are core's: validate() runs and its errors refuse the config") {
  // Fahrenheit with no temperature bands: the host does not check this itself (D-011), so the
  // error at /layers/temperature can only have come from validate().
  const ConfigLoadResult r = JsonConfigSource(tiny(R"(, "temperature_unit": "F")")).load();
  CHECK_FALSE(r.config.has_value());
  CHECK(has_issue(r, "/layers/temperature", IssueSeverity::Error));

  // A map area outside the canvas: also validate()'s.
  const ConfigLoadResult r2 =
      JsonConfigSource(
          tiny("", R"(, "map_area": {"x": 1000, "y": 0, "width": 960, "height": 800})"))
          .load();
  CHECK_FALSE(r2.config.has_value());
}

TEST_CASE("canvas.map_rotation: absent is 0, and a loaded config is already turned") {
  // tiny()'s pin sits at (0.5, 0.5), which no turn moves; move it off-centre so a turn shows.
  auto with_pin_at = [](std::string t) {
    t.replace(t.find("[0.5, 0.5]"), 10, "[0.25, 0.125]");
    return t;
  };
  SUBCASE("absent") {
    const ConfigLoadResult r = JsonConfigSource(with_pin_at(tiny())).load();
    REQUIRE(r.config.has_value());
    CHECK(r.config->canvas.map_rotation == 0);
    CHECK(r.config->pins[0].position.x == 0.25);
    CHECK(r.config->pins[0].position.y == 0.125);
  }
  SUBCASE("90: the source hands over the panel's frame (config.h, ConfigSource)") {
    const ConfigLoadResult r =
        JsonConfigSource(with_pin_at(tiny("", R"(, "map_rotation": 90)"))).load();
    REQUIRE(r.config.has_value());
    CHECK(r.config->canvas.map_rotation == 0);
    CHECK(r.config->pins[0].position.x == 0.875);  // (1 - y, x)
    CHECK(r.config->pins[0].position.y == 0.25);
  }
  SUBCASE("270") {
    const ConfigLoadResult r =
        JsonConfigSource(with_pin_at(tiny("", R"(, "map_rotation": 270)"))).load();
    REQUIRE(r.config.has_value());
    CHECK(r.config->pins[0].position.x == 0.125);  // (y, 1 - x)
    CHECK(r.config->pins[0].position.y == 0.75);
  }
  SUBCASE("45 is validate()'s error") {
    const ConfigLoadResult r = JsonConfigSource(tiny("", R"(, "map_rotation": 45)")).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/canvas/map_rotation", IssueSeverity::Error));
  }
  SUBCASE("a string is the parser's error") {
    const ConfigLoadResult r = JsonConfigSource(tiny("", R"(, "map_rotation": "90")")).load();
    CHECK_FALSE(r.config.has_value());
    CHECK(has_issue(r, "/canvas/map_rotation", IssueSeverity::Error));
  }
}

TEST_CASE("RFC 3339 timestamps") {
  CHECK(parse_rfc3339("1970-01-01T00:00:00Z").value_or(-1) == 0);
  CHECK(parse_rfc3339("2026-01-15T09:00:00Z").value_or(-1) == 1768467600);
  CHECK(parse_rfc3339("2026-01-15T19:00:00+10:00").value_or(-1) == 1768467600);
  CHECK(parse_rfc3339("2026-01-15T08:30:00-00:30").value_or(-1) == 1768467600);
  CHECK(parse_rfc3339("2026-01-15T09:00:00.75Z").value_or(-1) == 1768467600);
  CHECK(parse_rfc3339("2024-02-29T00:00:00Z").value_or(-1) == 1709164800);
  CHECK_FALSE(parse_rfc3339("2026-01-15 09:00:00Z").has_value());
  CHECK_FALSE(parse_rfc3339("2026-01-15T09:00:00").has_value());
  CHECK_FALSE(parse_rfc3339("2026-13-15T09:00:00Z").has_value());
  CHECK_FALSE(parse_rfc3339("2026-01-15T09:00:00Zjunk").has_value());
  CHECK_FALSE(parse_rfc3339("2026-01-15T09:00:00.Z").has_value());
}

TEST_CASE("readings fixtures: numbers, unavailable, unknown, timestamps as given") {
  const ReadingsLoadResult r = parse_readings(R"({
    "now": "2026-01-15T09:00:00Z",
    "readings": {
      "sensor.example_a": {"value": 21.5, "unit": "°C", "last_updated": "2026-01-15T08:55:00Z"},
      "sensor.example_b": {"value": "unavailable", "last_updated": "2026-01-15T08:00:00Z"},
      "sensor.example_c": {"value": "unknown", "last_updated": "2030-01-01T00:00:00Z"}
    }})");
  REQUIRE(r.fixture.has_value());
  CHECK(r.fixture->now == 1768467600);
  const Reading* a = r.fixture->readings.find("sensor.example_a");
  REQUIRE(a != nullptr);
  CHECK(a->state == ReadingState::Value);
  CHECK(a->value == doctest::Approx(21.5));
  CHECK(a->unit == "°C");
  CHECK(a->last_updated == 1768467300);
  CHECK(r.fixture->readings.find("sensor.example_b")->state == ReadingState::Unavailable);
  const Reading* c = r.fixture->readings.find("sensor.example_c");
  CHECK(c->state == ReadingState::Unknown);
  CHECK(c->last_updated == 1893456000);  // in the future: passed through, not clamped
  CHECK(r.fixture->readings.find("sensor.example_missing") == nullptr);

  CHECK_FALSE(parse_readings(R"({"now": "x", "readings": {}})").fixture.has_value());
  CHECK_FALSE(parse_readings(R"({"now": "2026-01-15T09:00:00Z", "readings": {
      "s": {"value": "warm", "last_updated": "2026-01-15T09:00:00Z"}}})")
                  .fixture.has_value());
  CHECK_FALSE(parse_readings("[").fixture.has_value());
}

TEST_CASE("every example fixture parses") {
  for (const char* name : {"all-ok", "thirsty-many", "stale-sensors", "humid-bathroom"}) {
    const auto r = parse_readings(
        slurp(std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/readings/" + name + ".json"));
    CHECK_MESSAGE(r.fixture.has_value(), name << ": " << r.error);
  }
}

TEST_CASE("integers the destination cannot hold are load errors, never narrowed") {
  // Review finding: 4294967344 narrowed to 48 and passed validate().
  const ConfigLoadResult cols =
      JsonConfigSource(tiny(R"(, "heat": {"grid_cols": 4294967344})")).load();
  CHECK_FALSE(cols.config.has_value());
  CHECK(has_issue(cols, "/heat/grid_cols", IssueSeverity::Error));

  std::string t = tiny();
  t.replace(t.find("\"version\": 1"), 12, "\"version\": 4294967297");
  const ConfigLoadResult version = JsonConfigSource(t).load();
  CHECK_FALSE(version.config.has_value());
  CHECK(has_issue(version, "/version", IssueSeverity::Error));

  const ConfigLoadResult huge =
      JsonConfigSource(tiny(R"(, "stale_after_s": 18446744073709551615)")).load();
  CHECK_FALSE(huge.config.has_value());
  CHECK(has_issue(huge, "/stale_after_s", IssueSeverity::Error));

  const ConfigLoadResult negative =
      JsonConfigSource(tiny(R"(, "hud": {"max_items": -9999999999})")).load();
  CHECK_FALSE(negative.config.has_value());
  CHECK(has_issue(negative, "/hud/max_items", IssueSeverity::Error));

  // Positive control: in range is fine.
  CHECK(JsonConfigSource(tiny(R"(, "heat": {"grid_cols": 64})")).load().config.has_value());
}

TEST_CASE("integral numbers spelled as floats are integers; fractions are not") {
  // Review finding: JSON Schema's integer type accepts 1280.0 and 4.8e1.
  std::string t = tiny(R"(, "heat": {"grid_cols": 4.8e1}, "stale_after_s": 3600.0)");
  t.replace(t.find("\"width\": 1280"), 13, "\"width\": 1280.0");
  t.replace(t.find("\"version\": 1"), 12, "\"version\": 1.0");
  const ConfigLoadResult ok = JsonConfigSource(t).load();
  REQUIRE(ok.config.has_value());
  CHECK(ok.config->canvas.width == 1280);
  CHECK(ok.config->heat.grid_cols == 48);
  CHECK(ok.config->stale_after == 3600);

  const ConfigLoadResult frac = JsonConfigSource(tiny(R"(, "heat": {"grid_cols": 48.5})")).load();
  CHECK_FALSE(frac.config.has_value());
  CHECK(has_issue(frac, "/heat/grid_cols", IssueSeverity::Error));

  const ConfigLoadResult big_float =
      JsonConfigSource(tiny(R"(, "heat": {"grid_rows": 1e12})")).load();
  CHECK_FALSE(big_float.config.has_value());
  CHECK(has_issue(big_float, "/heat/grid_rows", IssueSeverity::Error));
}

TEST_CASE("numbers beyond double range are load errors, not crashes") {
  const ConfigLoadResult r = JsonConfigSource(tiny(R"(, "stale_after_s": 1e400)")).load();
  CHECK_FALSE(r.config.has_value());
  CHECK_FALSE(r.issues.empty());
  const ReadingsLoadResult f = parse_readings(R"({"now": "2026-01-15T09:00:00Z", "readings": {
      "s": {"value": 1e400, "last_updated": "2026-01-15T09:00:00Z"}}})");
  CHECK_FALSE(f.fixture.has_value());
  CHECK_FALSE(f.error.empty());
}

TEST_CASE("RFC 3339: impossible dates and offsets are rejected") {
  CHECK_FALSE(parse_rfc3339("2026-02-29T09:00:00Z").has_value());  // not a leap year
  CHECK_FALSE(parse_rfc3339("2026-04-31T09:00:00Z").has_value());
  CHECK(parse_rfc3339("2028-02-29T00:00:00Z").has_value());        // a leap year
  CHECK(parse_rfc3339("2000-02-29T00:00:00Z").has_value());        // divisible by 400
  CHECK_FALSE(parse_rfc3339("2100-02-29T00:00:00Z").has_value());  // divisible by 100
  CHECK_FALSE(parse_rfc3339("2026-01-15T09:00:00+24:00").has_value());
  CHECK_FALSE(parse_rfc3339("2026-01-15T09:00:00+10:60").has_value());
  CHECK(parse_rfc3339("2026-01-15T09:00:00+23:59").has_value());
}
