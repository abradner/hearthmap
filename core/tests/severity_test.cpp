// severity.h: assess_freshness(), classify(), assess().
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "flat_fixture.h"
#include "hearthmap/config.h"
#include "hearthmap/readings.h"
#include "hearthmap/severity.h"

using namespace hearthmap;
using namespace hearthmap::test;

namespace {
constexpr Timestamp kNow = 1'000'000;
Reading val(double v, Timestamp t) {
  return value_reading(v, t);
}
}  // namespace

// --- assess_freshness ------------------------------------------------------------------------

TEST_CASE("freshness: missing, unavailable and unknown are distinct reasons") {
  CHECK(assess_freshness(nullptr, kNow, 60) == Freshness::Missing);
  Reading r = val(1, kNow);
  r.state = ReadingState::Unavailable;
  CHECK(assess_freshness(&r, kNow, 60) == Freshness::Unavailable);
  r.state = ReadingState::Unknown;
  CHECK(assess_freshness(&r, kNow, 60) == Freshness::Unknown);
  // An unavailable reading is unavailable even if its timestamp is also old.
  r.state = ReadingState::Unavailable;
  r.last_updated = 0;
  CHECK(assess_freshness(&r, kNow, 60) == Freshness::Unavailable);
}

TEST_CASE("freshness: exactly the timeout is fresh, one second more is too old") {
  const Reading at = val(1, kNow - 60);
  const Reading past = val(1, kNow - 61);
  const Reading just = val(1, kNow - 59);
  CHECK(assess_freshness(&just, kNow, 60) == Freshness::Fresh);
  CHECK(assess_freshness(&at, kNow, 60) == Freshness::Fresh);
  CHECK(assess_freshness(&past, kNow, 60) == Freshness::TooOld);
}

TEST_CASE("freshness: a future timestamp counts as age 0") {
  const Reading future = val(1, kNow + 3600);
  CHECK(assess_freshness(&future, kNow, 60) == Freshness::Fresh);
  const Reading far_future = val(1, std::numeric_limits<Timestamp>::max());
  CHECK(assess_freshness(&far_future, kNow, 1) == Freshness::Fresh);
}

TEST_CASE("freshness: extreme timestamps do not overflow") {
  const Reading ancient = val(1, std::numeric_limits<Timestamp>::min());
  CHECK(assess_freshness(&ancient, std::numeric_limits<Timestamp>::max(),
                         std::numeric_limits<Seconds>::max()) == Freshness::TooOld);
  // now - min at now = -1 is exactly max(): the boundary, still fresh.
  CHECK(assess_freshness(&ancient, -1, std::numeric_limits<Seconds>::max()) == Freshness::Fresh);
  CHECK(assess_freshness(&ancient, 0, std::numeric_limits<Seconds>::max()) == Freshness::TooOld);
}

TEST_CASE("freshness: a non-finite Value slipping through is Unknown") {
  Reading r = val(std::numeric_limits<double>::quiet_NaN(), kNow);
  CHECK(assess_freshness(&r, kNow, 60) == Freshness::Unknown);
  r.value = std::numeric_limits<double>::infinity();
  CHECK(assess_freshness(&r, kNow, 60) == Freshness::Unknown);
}

// --- classify --------------------------------------------------------------------------------

TEST_CASE("classify: on a threshold is the less severe side") {
  const Band b = default_band(Layer::Temperature);  // danger <12 >32, warn <16 >27
  CHECK(classify(b, 12.0) == Severity::Warn);
  CHECK(classify(b, 11.999) == Severity::Danger);
  CHECK(classify(b, 16.0) == Severity::Ok);
  CHECK(classify(b, 15.999) == Severity::Warn);
  CHECK(classify(b, 27.0) == Severity::Ok);
  CHECK(classify(b, 27.001) == Severity::Warn);
  CHECK(classify(b, 32.0) == Severity::Warn);
  CHECK(classify(b, 32.001) == Severity::Danger);
}

TEST_CASE("classify: comfort band does not affect severity; absent thresholds never match") {
  const Band b = default_band(Layer::Temperature);
  CHECK(classify(b, 17.0) == Severity::Ok);     // outside comfort 19..24, inside warn edges
  const Band soil = default_band(Layer::Soil);  // no below thresholds
  CHECK(classify(soil, -100.0) == Severity::Ok);
  Band none;
  CHECK(classify(none, 1e300) == Severity::Ok);
}

TEST_CASE("classify: danger is tested before warn on each side") {
  Band b;
  b.warn_above = 10;
  b.danger_above = 10;  // coincident edges
  CHECK(classify(b, 10.5) == Severity::Danger);
  Band only_danger;
  only_danger.danger_below = 0;
  CHECK(classify(only_danger, -1) == Severity::Danger);
  CHECK(classify(only_danger, 0) == Severity::Ok);
}

// --- assess ----------------------------------------------------------------------------------

namespace {
Config one_room(bool outdoor = false) {
  Config c;
  Room r;
  r.id = "room";
  r.name = "Room";
  r.polygon = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  r.outdoor = outdoor;
  c.rooms.push_back(r);
  return c;
}
Pin make_pin(PinKind kind, std::initializer_list<EntityBinding> e) {
  Pin p;
  p.id = "p";
  p.kind = kind;
  p.name = "P";
  p.position = {0.5, 0.5};
  p.room_id = "room";
  p.entities = e;
  return p;
}
}  // namespace

TEST_CASE("assess: nullopt when the pin does not bind the role") {
  const Config c = one_room();
  const Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  CHECK_FALSE(assess(c, p, Role::Humidity, Readings{}, kNow).has_value());
}

TEST_CASE("assess: fresh temperature is classified against the layer band") {
  const Config c = one_room();
  const Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  Readings r;
  r.by_entity["sensor.example_t"] = val(28.0, kNow - 10);
  const auto a = assess(c, p, Role::Temperature, r, kNow);
  REQUIRE(a);
  CHECK(a->freshness == Freshness::Fresh);
  CHECK(*a->value == 28.0);
  CHECK(*a->raw == 28.0);
  CHECK(a->severity == Severity::Warn);
  REQUIRE(a->band);
  CHECK(a->band->warn_above == 27.0);
}

TEST_CASE("assess: stale reasons give Stale, no value, but raw when a number exists") {
  const Config c = one_room();
  Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  p.stale_after = 60;
  Readings r;
  r.by_entity["sensor.example_t"] = val(40.0, kNow - 61);
  auto a = assess(c, p, Role::Temperature, r, kNow);
  REQUIRE(a);
  CHECK(a->freshness == Freshness::TooOld);
  CHECK(a->severity == Severity::Stale);  // not Danger, though 40 is beyond danger_above
  CHECK_FALSE(a->value.has_value());
  CHECK(*a->raw == 40.0);

  r.by_entity["sensor.example_t"].last_updated = kNow - 60;  // exactly the pin's timeout
  a = assess(c, p, Role::Temperature, r, kNow);
  CHECK(a->freshness == Freshness::Fresh);
  CHECK(a->severity == Severity::Danger);

  r.by_entity["sensor.example_t"].state = ReadingState::Unavailable;
  a = assess(c, p, Role::Temperature, r, kNow);
  CHECK(a->freshness == Freshness::Unavailable);
  CHECK_FALSE(a->raw.has_value());

  r.by_entity.clear();
  a = assess(c, p, Role::Temperature, r, kNow);
  CHECK(a->freshness == Freshness::Missing);
  CHECK(a->severity == Severity::Stale);
}

TEST_CASE("assess: the global stale timeout applies without a pin override") {
  Config c = one_room();
  c.stale_after = 100;
  const Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  Readings r;
  r.by_entity["sensor.example_t"] = val(20.0, kNow - 100);
  CHECK(assess(c, p, Role::Temperature, r, kNow)->freshness == Freshness::Fresh);
  r.by_entity["sensor.example_t"].last_updated = kNow - 101;
  CHECK(assess(c, p, Role::Temperature, r, kNow)->freshness == Freshness::TooOld);
}

TEST_CASE("assess: moisture is thirst against the pin's effective soil band") {
  const Config c = one_room();
  Pin p = make_pin(PinKind::Plant, {{Role::Moisture, "sensor.example_m"}});
  p.calibration = Calibration{};
  p.calibration->dry = 0;
  p.calibration->wet = 100;
  p.calibration->happy_min = 50;  // warn above thirst 0.5
  Readings r;
  r.by_entity["sensor.example_m"] = val(40.0, kNow);
  auto a = assess(c, p, Role::Moisture, r, kNow);
  REQUIRE(a);
  CHECK(*a->value == doctest::Approx(0.6));
  CHECK(*a->raw == 40.0);
  CHECK(a->severity == Severity::Warn);
  CHECK(*a->band->warn_above == doctest::Approx(0.5));

  r.by_entity["sensor.example_m"].value = 50.0;  // exactly on the happy edge
  CHECK(assess(c, p, Role::Moisture, r, kNow)->severity == Severity::Ok);
  r.by_entity["sensor.example_m"].value = 0.0;  // bone dry, a real reading
  a = assess(c, p, Role::Moisture, r, kNow);
  CHECK(*a->value == 1.0);
  CHECK(a->severity == Severity::Danger);
}

TEST_CASE("assess: degenerate or absent calibration is BadCalibration, never a guess") {
  const Config c = one_room();
  Pin p = make_pin(PinKind::Plant, {{Role::Moisture, "sensor.example_m"}});
  p.calibration = Calibration{};
  p.calibration->dry = 30;
  p.calibration->wet = 30;
  Readings r;
  r.by_entity["sensor.example_m"] = val(30.0, kNow);
  auto a = assess(c, p, Role::Moisture, r, kNow);
  REQUIRE(a);
  CHECK(a->freshness == Freshness::BadCalibration);
  CHECK(a->severity == Severity::Stale);
  CHECK_FALSE(a->value.has_value());
  CHECK(*a->raw == 30.0);
  // Precedence: Missing > Unavailable > Unknown > BadCalibration > TooOld.
  r.by_entity["sensor.example_m"].last_updated = kNow - 999'999;
  CHECK(assess(c, p, Role::Moisture, r, kNow)->freshness == Freshness::BadCalibration);
  r.by_entity["sensor.example_m"].state = ReadingState::Unknown;
  CHECK(assess(c, p, Role::Moisture, r, kNow)->freshness == Freshness::Unknown);
  r.by_entity["sensor.example_m"].state = ReadingState::Unavailable;
  CHECK(assess(c, p, Role::Moisture, r, kNow)->freshness == Freshness::Unavailable);
  r.by_entity.clear();
  CHECK(assess(c, p, Role::Moisture, r, kNow)->freshness == Freshness::Missing);
  // Absent calibration (an invalid config) is treated the same.
  p.calibration.reset();
  r.by_entity["sensor.example_m"] = val(30.0, kNow);
  CHECK(assess(c, p, Role::Moisture, r, kNow)->freshness == Freshness::BadCalibration);
}

TEST_CASE("assess: outdoor rooms exempt temperature, humidity and CO2 but not moisture") {
  const Config c = one_room(true);
  Pin clim = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"},
                                             {Role::Humidity, "sensor.example_h"},
                                             {Role::Co2, "sensor.example_c"}});
  Readings r;
  r.by_entity["sensor.example_t"] = val(-5.0, kNow);
  r.by_entity["sensor.example_h"] = val(99.0, kNow);
  r.by_entity["sensor.example_c"] = val(5000.0, kNow);
  for (Role role : {Role::Temperature, Role::Humidity, Role::Co2}) {
    CAPTURE(static_cast<int>(role));
    const auto a = assess(c, clim, role, r, kNow);
    CHECK(a->severity == Severity::Ok);
    CHECK(a->value.has_value());  // still coloured on the heat field
  }
  // Stale is still stale outdoors.
  r.by_entity.erase("sensor.example_t");
  CHECK(assess(c, clim, Role::Temperature, r, kNow)->severity == Severity::Stale);

  Pin plant = make_pin(PinKind::Plant, {{Role::Moisture, "sensor.example_m"}});
  plant.calibration = Calibration{};
  r.by_entity["sensor.example_m"] = val(0.0, kNow);
  CHECK(assess(c, plant, Role::Moisture, r, kNow)->severity == Severity::Danger);

  // The same readings indoors are judged.
  const Config indoor = one_room(false);
  r.by_entity["sensor.example_t"] = val(-5.0, kNow);
  CHECK(assess(indoor, clim, Role::Temperature, r, kNow)->severity == Severity::Danger);
}

TEST_CASE("assess: display-only and non-layer roles are Ok when fresh and carry no band") {
  const Config c = one_room();
  Pin p = make_pin(PinKind::Plant, {{Role::Moisture, "sensor.example_m"},
                                    {Role::Temperature, "sensor.example_t"},
                                    {Role::Battery, "sensor.example_b"}});
  p.calibration = Calibration{};
  Readings r;
  r.by_entity["sensor.example_t"] = val(45.0, kNow);  // would be Danger on a climate pin
  r.by_entity["sensor.example_b"] = val(3.0, kNow);
  for (Role role : {Role::Temperature, Role::Battery}) {
    CAPTURE(static_cast<int>(role));
    const auto a = assess(c, p, role, r, kNow);
    REQUIRE(a);
    CHECK(a->freshness == Freshness::Fresh);
    CHECK(a->severity == Severity::Ok);
    CHECK_FALSE(a->band.has_value());
    CHECK(a->value.has_value());
  }
  r.by_entity.erase("sensor.example_b");
  CHECK(assess(c, p, Role::Battery, r, kNow)->severity == Severity::Stale);
}

TEST_CASE("assess: an unknown room id is judged as indoor") {
  Config c = one_room(true);
  Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  p.room_id = "nowhere";
  Readings r;
  r.by_entity["sensor.example_t"] = val(40.0, kNow);
  CHECK(assess(c, p, Role::Temperature, r, kNow)->severity == Severity::Danger);
}

TEST_CASE("assess on the flat's stale-sensors fixture") {
  const Config c = flat_config();
  const Readings r = flat_stale_sensors();
  auto fresh = [&](const char* pin, Role role) {
    return assess(c, c.pins[pin_index(c, pin)], role, r, kFlatNow)->freshness;
  };
  CHECK(fresh("clim_kitchen", Role::Temperature) == Freshness::TooOld);
  CHECK(fresh("clim_study", Role::Temperature) == Freshness::TooOld);
  CHECK(fresh("hum_bathroom", Role::Humidity) == Freshness::Unavailable);
  CHECK(fresh("plant_snake", Role::Moisture) == Freshness::Unknown);
  CHECK(fresh("air_bedroom", Role::Co2) == Freshness::Missing);
  CHECK(fresh("plant_tomato", Role::Moisture) == Freshness::Fresh);    // 4 h under a 6 h override
  CHECK(fresh("plant_lavender", Role::Moisture) == Freshness::Fresh);  // exactly 3 h
}

TEST_CASE("assess: roles feeding a disabled layer are not judged, but still reported (D-020)") {
  Config c = one_room();
  c.layers[index_of(Layer::Temperature)].enabled = false;
  Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  Readings r;
  r.by_entity["sensor.example_t"] = val(40.0, kNow);
  auto a = assess(c, p, Role::Temperature, r, kNow);
  REQUIRE(a);
  CHECK(a->severity == Severity::Ok);
  CHECK(a->freshness == Freshness::Fresh);
  CHECK(*a->value == 40.0);
  REQUIRE(a->band.has_value());  // the band is still filled
  CHECK(a->band->danger_above == 32.0);
  // Stale is still reported as Stale.
  r.by_entity.clear();
  CHECK(assess(c, p, Role::Temperature, r, kNow)->severity == Severity::Stale);

  // Soil disabled: moisture is unjudged but its effective band and thirst are still there.
  Config s = one_room();
  s.layers[index_of(Layer::Soil)].enabled = false;
  Pin plant = make_pin(PinKind::Plant, {{Role::Moisture, "sensor.example_m"}});
  plant.calibration = Calibration{};
  plant.calibration->happy_min = 50;
  r.by_entity["sensor.example_m"] = val(0.0, kNow);
  a = assess(s, plant, Role::Moisture, r, kNow);
  CHECK(a->severity == Severity::Ok);
  CHECK(*a->value == 1.0);
  CHECK(*a->band->warn_above == doctest::Approx(0.5));
}

TEST_CASE("assess: band is filled even when the reading is not fresh") {
  const Config c = one_room();
  const Pin p = make_pin(PinKind::Temperature, {{Role::Temperature, "sensor.example_t"}});
  const auto a = assess(c, p, Role::Temperature, Readings{}, kNow);
  REQUIRE(a->band.has_value());
  CHECK(a->band->warn_above == 27.0);
}
