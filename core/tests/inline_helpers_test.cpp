// Tests for the inline helpers that ship in core's headers in PR 1. Everything declared but
// not defined in the headers is tested by the PR that implements it.
#include <doctest/doctest.h>

#include <cstdint>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/geometry.h"
#include "hearthmap/readings.h"
#include "hearthmap/severity.h"
#include "hearthmap/view_model.h"

using namespace hearthmap;

TEST_CASE("to_rgb565 packs the high bits of each channel") {
  CHECK(to_rgb565(Rgb888{0, 0, 0}) == 0x0000);
  CHECK(to_rgb565(Rgb888{0xFF, 0xFF, 0xFF}) == 0xFFFF);
  CHECK(to_rgb565(Rgb888{0xFF, 0, 0}) == 0xF800);
  CHECK(to_rgb565(Rgb888{0, 0xFF, 0}) == 0x07E0);
  CHECK(to_rgb565(Rgb888{0, 0, 0xFF}) == 0x001F);
  // Low bits are truncated, not rounded: 0x07 is below the red channel's resolution.
  CHECK(to_rgb565(Rgb888{0x07, 0x03, 0x07}) == 0x0000);
}

TEST_CASE("from_rgb565 round-trips every 16-bit colour") {
  for (std::uint32_t v = 0; v <= 0xFFFFU; ++v) {
    const auto c = static_cast<Rgb565>(v);
    REQUIRE(to_rgb565(from_rgb565(c)) == c);
  }
  const Rgb888 white = from_rgb565(0xFFFF);
  CHECK(white.r == 0xFF);
  CHECK(white.g == 0xFF);
  CHECK(white.b == 0xFF);
}

TEST_CASE("lerp clamps t and rounds to nearest") {
  const Rgb888 a{0, 100, 200};
  const Rgb888 b{255, 0, 100};
  const Rgb888 lo = lerp(a, b, -1.0);
  CHECK(lo.r == 0);
  CHECK(lo.g == 100);
  const Rgb888 hi = lerp(a, b, 2.0);
  CHECK(hi.r == 255);
  CHECK(hi.b == 100);
  const Rgb888 mid = lerp(a, b, 0.5);
  CHECK(mid.r == 128);  // 127.5 rounds up
  CHECK(mid.g == 50);
  CHECK(mid.b == 150);
  // Usable at compile time: ui_lvgl may build palette tables as constants.
  static_assert(to_rgb565(lerp(Rgb888{0, 0, 0}, Rgb888{255, 255, 255}, 1.0)) == 0xFFFF);
}

TEST_CASE("map and canvas conversions are inverse over the map area") {
  const CanvasRect area{0, 0, 960, 800};
  const CanvasPoint c = to_canvas(area, MapPoint{0.5, 0.25});
  CHECK(c.x == doctest::Approx(480.0));
  CHECK(c.y == doctest::Approx(200.0));
  const MapPoint m = to_map(area, c);
  CHECK(m.x == doctest::Approx(0.5));
  CHECK(m.y == doctest::Approx(0.25));

  const CanvasRect offset{100, 50, 200, 100};
  const CanvasPoint o = to_canvas(offset, MapPoint{1.0, 1.0});
  CHECK(o.x == doctest::Approx(300.0));
  CHECK(o.y == doctest::Approx(150.0));
}

TEST_CASE("contains is inclusive on left/top and exclusive on right/bottom") {
  const CanvasRect r{10, 20, 100, 50};
  CHECK(contains(r, CanvasPoint{10.0, 20.0}));
  CHECK(contains(r, CanvasPoint{109.9, 69.9}));
  CHECK_FALSE(contains(r, CanvasPoint{110.0, 30.0}));
  CHECK_FALSE(contains(r, CanvasPoint{50.0, 70.0}));
  CHECK_FALSE(contains(r, CanvasPoint{9.9, 30.0}));
}

TEST_CASE("every pin kind has a primary role that feeds a layer") {
  for (PinKind k : {PinKind::Temperature, PinKind::Humidity, PinKind::Soil, PinKind::AirQuality,
                    PinKind::Plant}) {
    CAPTURE(static_cast<int>(k));
    CHECK(feeds_layer(k, primary_role(k)));
  }
  CHECK(primary_role(PinKind::Plant) == Role::Moisture);
  CHECK(primary_role(PinKind::AirQuality) == Role::Co2);
}

TEST_CASE("soil and plant pins' temperature and humidity are display-only") {
  CHECK_FALSE(feeds_layer(PinKind::Plant, Role::Temperature));
  CHECK_FALSE(feeds_layer(PinKind::Plant, Role::Humidity));
  CHECK_FALSE(feeds_layer(PinKind::Soil, Role::Temperature));
  CHECK(feeds_layer(PinKind::Humidity, Role::Temperature));
  CHECK(feeds_layer(PinKind::AirQuality, Role::Humidity));
  CHECK_FALSE(feeds_layer(PinKind::Temperature, Role::Battery));
  CHECK_FALSE(feeds_layer(PinKind::Plant, Role::PredictedDryAt));
}

TEST_CASE("moisture feeds the soil layer only from soil and plant pins") {
  CHECK(feeds_layer(PinKind::Soil, Role::Moisture));
  CHECK(feeds_layer(PinKind::Plant, Role::Moisture));
  CHECK_FALSE(feeds_layer(PinKind::Temperature, Role::Moisture));
  CHECK_FALSE(feeds_layer(PinKind::Humidity, Role::Moisture));
  CHECK_FALSE(feeds_layer(PinKind::AirQuality, Role::Moisture));
}

TEST_CASE("band field bits are distinct and kAllBandFields covers all eight") {
  unsigned all = 0;
  for (std::size_t i = 0; i < kBandFieldCount; ++i) {
    const auto bit = band_field_bit(static_cast<BandField>(i));
    CHECK((all & bit) == 0U);
    all |= bit;
  }
  CHECK(all == kAllBandFields);
}

TEST_CASE("the air layer's default scale starts below its comfort band") {
  const Band air = default_band(Layer::Air);
  CHECK(air.scale_min == doctest::Approx(350.0));
  CHECK(air.comfort_min == doctest::Approx(400.0));
  CHECK(air.scale_min < air.comfort_min);
}

TEST_CASE("to_float keeps a map point's coordinates") {
  const MapPointF f = to_float(MapPoint{0.25, 0.75});
  CHECK(f.x == doctest::Approx(0.25F));
  CHECK(f.y == doctest::Approx(0.75F));
}

namespace {
// Band's documented ordering for every threshold that is present.
bool ordered(const Band& b) {
  double prev = b.scale_min;
  bool ok = true;
  auto step = [&](double v) {
    ok = ok && prev <= v;
    prev = v;
  };
  if (b.danger_below) step(*b.danger_below);
  if (b.warn_below) step(*b.warn_below);
  step(b.comfort_min);
  step(b.comfort_max);
  if (b.warn_above) step(*b.warn_above);
  if (b.danger_above) step(*b.danger_above);
  step(b.scale_max);
  return ok && b.scale_min < b.scale_max;
}
}  // namespace

TEST_CASE("default bands satisfy Band's ordering invariant") {
  for (Layer l : {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air}) {
    CAPTURE(static_cast<int>(l));
    CHECK(ordered(default_band(l)));
  }
  // The invariant check itself can fail (positive control).
  Band broken = default_band(Layer::Temperature);
  broken.warn_above = 20.0;  // inside comfort
  CHECK_FALSE(ordered(broken));
}

TEST_CASE("a default-constructed Config carries the documented defaults") {
  const Config c;
  CHECK(c.version == kSchemaVersion);
  CHECK(c.canvas.width == 1280);
  CHECK(c.canvas.height == 800);
  // The default map area is the whole default canvas (D-023).
  CHECK(c.canvas.map_area.x == 0);
  CHECK(c.canvas.map_area.y == 0);
  CHECK(c.canvas.map_area.width == c.canvas.width);
  CHECK(c.canvas.map_area.height == c.canvas.height);
  CHECK(c.stale_after == 10800);
  CHECK(c.hud.max_items == 6);
  for (Layer l : {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air}) {
    CHECK(c.layer(l).enabled);
    CHECK(c.layer(l).band.comfort_max == default_band(l).comfort_max);
    CHECK(c.layer(l).present == 0);
  }
  const UiState s = initial_ui_state(c);
  CHECK(s.active_layer == c.default_layer);
  CHECK_FALSE(s.overlay_pin.has_value());
}

TEST_CASE("severity rank puts stale between ok and warn") {
  CHECK(worse(Severity::Ok, Severity::Stale) == Severity::Stale);
  CHECK(worse(Severity::Stale, Severity::Warn) == Severity::Warn);
  CHECK(worse(Severity::Danger, Severity::Warn) == Severity::Danger);
  CHECK(worse(Severity::Ok, Severity::Ok) == Severity::Ok);
}

TEST_CASE("find_binding and Readings::find return nullptr when absent") {
  Pin p;
  p.entities.push_back(EntityBinding{Role::Temperature, "sensor.example_t"});
  REQUIRE(find_binding(p, Role::Temperature) != nullptr);
  CHECK(find_binding(p, Role::Temperature)->entity_id == "sensor.example_t");
  CHECK(find_binding(p, Role::Humidity) == nullptr);

  Readings r;
  r.by_entity["sensor.example_t"] = Reading{ReadingState::Value, 21.5, "°C", 100};
  REQUIRE(r.find("sensor.example_t") != nullptr);
  CHECK(r.find("sensor.example_t")->value == doctest::Approx(21.5));
  CHECK(r.find("sensor.missing") == nullptr);
}

TEST_CASE("pin override wins over the global stale timeout") {
  Config c;
  Pin p;
  CHECK(effective_stale_after(p, c) == 10800);
  p.stale_after = 600;
  CHECK(effective_stale_after(p, c) == 600);
}

TEST_CASE("UiEvent factories set the type and payload") {
  CHECK(UiEvent::tap_map(CanvasPoint{1.0, 2.0}).type == UiEvent::Type::TapMap);
  CHECK(UiEvent::select_layer(Layer::Soil).layer == Layer::Soil);
  CHECK(UiEvent::tap_hud_item("plant_basil").pin_id == "plant_basil");
  CHECK(UiEvent::close_overlay().type == UiEvent::Type::CloseOverlay);
}
