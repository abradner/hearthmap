// calibration.h: moisture_fraction(), thirst(), effective_soil_band().
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "hearthmap/calibration.h"
#include "hearthmap/config.h"
#include "hearthmap/severity.h"

using namespace hearthmap;

namespace {
Calibration cal(double dry, double wet) {
  Calibration c;
  c.dry = dry;
  c.wet = wet;
  return c;
}
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
}  // namespace

TEST_CASE("moisture_fraction on a normal range: ends, middle and clamping") {
  const Calibration c = cal(10, 60);
  CHECK(*moisture_fraction(c, 10) == 0.0);
  CHECK(*moisture_fraction(c, 60) == 1.0);
  CHECK(*moisture_fraction(c, 35) == doctest::Approx(0.5));
  CHECK(*moisture_fraction(c, 0) == 0.0);    // drier than dry clamps
  CHECK(*moisture_fraction(c, 100) == 1.0);  // wetter than wet clamps
}

TEST_CASE("a raw 0 is a real reading, not missing") {
  const Calibration c = cal(0, 55);
  REQUIRE(thirst(c, 0.0).has_value());
  CHECK(*thirst(c, 0.0) == 1.0);
}

TEST_CASE("inverted ranges (wet < dry) need no special case") {
  const Calibration c = cal(2900, 1300);  // capacitive ADC: lower is wetter
  CHECK(*moisture_fraction(c, 2900) == 0.0);
  CHECK(*moisture_fraction(c, 1300) == 1.0);
  CHECK(*moisture_fraction(c, 2100) == doctest::Approx(0.5));
  CHECK(*moisture_fraction(c, 3500) == 0.0);  // drier than dry
  CHECK(*moisture_fraction(c, 1000) == 1.0);  // wetter than wet
  CHECK(*thirst(c, 2750) == doctest::Approx(1.0 - 150.0 / 1600.0));
}

TEST_CASE("zero-width calibration gives nullopt, never a confident 0 or 1") {
  CHECK_FALSE(moisture_fraction(cal(30, 30), 30).has_value());
  CHECK_FALSE(thirst(cal(30, 30), 0).has_value());
  // Just under the width threshold is still degenerate; just over is usable.
  CHECK_FALSE(moisture_fraction(cal(0, 0.5e-9), 0).has_value());
  CHECK(moisture_fraction(cal(0, 2e-9), 0).has_value());
  CHECK(moisture_fraction(cal(0, kMinCalibrationWidth), 0).has_value());  // exactly: usable
  CHECK_FALSE(moisture_fraction(cal(0, -0.5e-9), 0).has_value());         // inverted, tiny
}

TEST_CASE("non-finite inputs give nullopt") {
  const Calibration c = cal(0, 100);
  CHECK_FALSE(moisture_fraction(c, kNaN).has_value());
  CHECK_FALSE(moisture_fraction(c, kInf).has_value());
  CHECK_FALSE(moisture_fraction(c, -kInf).has_value());
  CHECK_FALSE(thirst(c, kNaN).has_value());
  CHECK_FALSE(moisture_fraction(cal(kNaN, 100), 50).has_value());
  CHECK_FALSE(moisture_fraction(cal(0, kInf), 50).has_value());
}

TEST_CASE("thirst is 1 - moisture_fraction") {
  const Calibration c = cal(5, 60);
  for (double raw : {-10.0, 5.0, 6.0, 32.5, 59.0, 60.0, 80.0}) {
    CAPTURE(raw);
    CHECK(*thirst(c, raw) == doctest::Approx(1.0 - *moisture_fraction(c, raw)));
  }
}

// --- effective_soil_band --------------------------------------------------------------------

namespace {
void check_ordered(const Band& b) {
  double prev = b.scale_min;
  auto step = [&](double v) {
    CHECK(prev <= v);
    prev = v;
  };
  if (b.danger_below) step(*b.danger_below);
  if (b.warn_below) step(*b.warn_below);
  step(b.comfort_min);
  step(b.comfort_max);
  if (b.warn_above) step(*b.warn_above);
  if (b.danger_above) step(*b.danger_above);
  step(b.scale_max);
}
}  // namespace

TEST_CASE("no happy range: the layer band is returned unchanged") {
  const Band layer = default_band(Layer::Soil);
  const Band b = effective_soil_band(layer, cal(10, 70));
  CHECK(b.comfort_min == layer.comfort_min);
  CHECK(b.comfort_max == layer.comfort_max);
  CHECK(b.warn_above == layer.warn_above);
  CHECK(b.danger_above == layer.danger_above);
  CHECK_FALSE(b.warn_below.has_value());
}

TEST_CASE("degenerate calibration: the layer band is returned unchanged") {
  Calibration c = cal(30, 30);
  c.happy_min = 30;
  c.happy_max = 30;
  const Band layer = default_band(Layer::Soil);
  const Band b = effective_soil_band(layer, c);
  CHECK(b.comfort_max == layer.comfort_max);
  CHECK(b.warn_above == layer.warn_above);
  CHECK(b.warn_below == layer.warn_below);
}

TEST_CASE("happy_min only: dry edge moves, danger_above never drops below it") {
  // fern: dry 15, wet 80, happy_min 40 -> thirst(40) = 1 - 25/65.
  Calibration c = cal(15, 80);
  c.happy_min = 40;
  const double t = 1.0 - 25.0 / 65.0;
  const Band b = effective_soil_band(default_band(Layer::Soil), c);
  CHECK(b.comfort_max == doctest::Approx(t));
  CHECK(*b.warn_above == doctest::Approx(t));
  CHECK(*b.danger_above == doctest::Approx(0.9));  // max(0.9, 0.615)
  CHECK(b.comfort_min == doctest::Approx(0.1));    // untouched, still below comfort_max
  CHECK_FALSE(b.warn_below.has_value());
  check_ordered(b);

  // A happy_min drier than the layer's danger edge lifts danger_above to it.
  Calibration dry = cal(0, 100);
  dry.happy_min = 5;  // thirst 0.95
  const Band d = effective_soil_band(default_band(Layer::Soil), dry);
  CHECK(*d.warn_above == doctest::Approx(0.95));
  CHECK(*d.danger_above == doctest::Approx(0.95));
  check_ordered(d);
}

TEST_CASE("happy_min only, wetter than the layer's comfort_min: comfort_min repaired") {
  Calibration c = cal(0, 100);
  c.happy_min = 95;  // thirst 0.05, below the default comfort_min 0.1
  const Band b = effective_soil_band(default_band(Layer::Soil), c);
  CHECK(b.comfort_max == doctest::Approx(0.05));
  CHECK(b.comfort_min == doctest::Approx(0.05));
  CHECK(*b.warn_above == doctest::Approx(0.05));
  check_ordered(b);
}

TEST_CASE("happy_max only: wet edge moves, danger_below follows the layer") {
  Calibration c = cal(0, 100);
  c.happy_max = 80;  // thirst 0.2
  const Band layer = default_band(Layer::Soil);
  const Band b = effective_soil_band(layer, c);
  CHECK(b.comfort_min == doctest::Approx(0.2));
  CHECK(*b.warn_below == doctest::Approx(0.2));
  CHECK_FALSE(b.danger_below.has_value());  // layer has none
  CHECK(b.comfort_max == doctest::Approx(0.6));
  check_ordered(b);

  Band with_danger = layer;
  with_danger.danger_below = 0.3;  // a layer danger edge drier than the happy edge
  with_danger.warn_below = 0.35;
  with_danger.comfort_min = 0.4;
  const Band d = effective_soil_band(with_danger, c);
  CHECK(*d.danger_below == doctest::Approx(0.2));  // min(0.3, 0.2)
  check_ordered(d);
}

TEST_CASE("happy_max only, drier than the layer's comfort_max: comfort_max and above repaired") {
  Calibration c = cal(0, 100);
  c.happy_max = 20;  // thirst 0.8: drier than comfort_max 0.6 and warn_above 0.7
  const Band b = effective_soil_band(default_band(Layer::Soil), c);
  CHECK(b.comfort_min == doctest::Approx(0.8));
  CHECK(b.comfort_max == doctest::Approx(0.8));
  CHECK(*b.warn_above == doctest::Approx(0.8));
  CHECK(*b.danger_above == doctest::Approx(0.9));
  check_ordered(b);
}

TEST_CASE("both happy edges on an inverted range") {
  Calibration c = cal(2900, 1300);
  c.happy_min = 2500;  // driest happy: thirst 1 - 400/1600 = 0.75
  c.happy_max = 1700;  // wettest happy: thirst 1 - 1200/1600 = 0.25
  const Band b = effective_soil_band(default_band(Layer::Soil), c);
  CHECK(b.comfort_min == doctest::Approx(0.25));
  CHECK(b.comfort_max == doctest::Approx(0.75));
  CHECK(*b.warn_below == doctest::Approx(0.25));
  CHECK(*b.warn_above == doctest::Approx(0.75));
  CHECK(*b.danger_above == doctest::Approx(0.9));
  check_ordered(b);
  // And the severities that follow from it, on the exact edges.
  CHECK(classify(b, 0.75) == Severity::Ok);
  CHECK(classify(b, 0.7500001) == Severity::Warn);
  CHECK(classify(b, 0.25) == Severity::Ok);
  CHECK(classify(b, 0.2499999) == Severity::Warn);
}

TEST_CASE("the scale widens to contain moved edges") {
  Band narrow = default_band(Layer::Soil);
  narrow.scale_min = 0.05;
  narrow.scale_max = 0.95;
  Calibration c = cal(0, 100);
  c.happy_max = 100;  // thirst 0
  c.happy_min = 0;    // thirst 1
  const Band b = effective_soil_band(narrow, c);
  CHECK(b.scale_min == 0.0);
  CHECK(b.scale_max == 1.0);
  check_ordered(b);
}
