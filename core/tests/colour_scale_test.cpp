// Tests for colour_for() / colour565_for() (colour_scale.h): quiet inside the comfort band,
// blending out to cold/hot, clamping beyond the scale, step sides, non-finite values.
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"

using namespace hearthmap;

namespace {

bool same(Rgb888 a, Rgb888 b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

// The UI spec's temperature mock: comfort 21.5..23.5 C, full blue at 18, full red at 27.5.
Band spec_temperature_band() {
  Band band = default_band(Layer::Temperature);
  band.comfort_min = 21.5;
  band.comfort_max = 23.5;
  band.scale_min = 18.0;
  band.scale_max = 27.5;
  return band;
}

const Palette kSpec{};

}  // namespace

TEST_CASE("Palette defaults are the UI spec's heatmap colours (D-025)") {
  CHECK(same(kSpec.neutral, Rgb888{30, 35, 41}));
  CHECK(same(kSpec.cold, Rgb888{42, 120, 214}));
  CHECK(same(kSpec.hot, Rgb888{227, 73, 72}));
}

TEST_CASE("colour_for: neutral throughout the comfort band, edges included") {
  const Band band = spec_temperature_band();
  CHECK(same(colour_for(band, 21.5), kSpec.neutral));
  CHECK(same(colour_for(band, 22.5), kSpec.neutral));
  CHECK(same(colour_for(band, 23.5), kSpec.neutral));
}

TEST_CASE("colour_for: the comfort edges are exact") {
  // Scale ends one ulp beyond the comfort edges, so one ulp past an edge is already the far
  // colour: the edge itself must still be neutral and its neighbour must not.
  Band band = spec_temperature_band();
  band.scale_min = std::nextafter(band.comfort_min, -1e9);
  band.scale_max = std::nextafter(band.comfort_max, 1e9);
  CHECK(same(colour_for(band, band.comfort_min), kSpec.neutral));
  CHECK(same(colour_for(band, band.comfort_max), kSpec.neutral));
  CHECK(same(colour_for(band, band.scale_min), kSpec.cold));
  CHECK(same(colour_for(band, band.scale_max), kSpec.hot));
}

TEST_CASE("colour_for: blends linearly towards cold below and hot above") {
  const Band band = spec_temperature_band();
  // Halfway between comfort_min (21.5) and scale_min (18).
  CHECK(same(colour_for(band, 19.75), lerp(kSpec.neutral, kSpec.cold, 0.5)));
  // A quarter of the way from comfort_max (23.5) to scale_max (27.5).
  CHECK(same(colour_for(band, 24.5), lerp(kSpec.neutral, kSpec.hot, 0.25)));
  // Saturates exactly at the scale ends.
  CHECK(same(colour_for(band, 18.0), kSpec.cold));
  CHECK(same(colour_for(band, 27.5), kSpec.hot));
}

TEST_CASE("colour_for: values far outside the scale clamp to the end colours") {
  const Band band = spec_temperature_band();
  CHECK(same(colour_for(band, -273.15), kSpec.cold));
  CHECK(same(colour_for(band, -1e300), kSpec.cold));
  CHECK(same(colour_for(band, 1000.0), kSpec.hot));
  CHECK(same(colour_for(band, 1e300), kSpec.hot));
  CHECK(same(colour_for(band, std::numeric_limits<double>::max()), kSpec.hot));
  CHECK(same(colour_for(band, std::numeric_limits<double>::lowest()), kSpec.cold));
}

TEST_CASE("colour_for: a side with no gradient room stays neutral (D-021)") {
  Band band = spec_temperature_band();
  SUBCASE("scale_min == comfort_min: anything below is neutral") {
    band.scale_min = band.comfort_min;
    CHECK(same(colour_for(band, 21.4), kSpec.neutral));
    CHECK(same(colour_for(band, -1e9), kSpec.neutral));
    CHECK(same(colour_for(band, 30.0), kSpec.hot));  // the other side is unaffected
  }
  SUBCASE("scale_min above comfort_min (invalid, still defined): neutral") {
    band.scale_min = 22.0;
    CHECK(same(colour_for(band, 21.4), kSpec.neutral));
  }
  SUBCASE("scale_max == comfort_max: anything above is neutral") {
    band.scale_max = band.comfort_max;
    CHECK(same(colour_for(band, 23.6), kSpec.neutral));
    CHECK(same(colour_for(band, 1e9), kSpec.neutral));
    CHECK(same(colour_for(band, 10.0), kSpec.cold));
  }
  SUBCASE("scale_max below comfort_max (invalid, still defined): neutral") {
    band.scale_max = 20.0;
    CHECK(same(colour_for(band, 23.6), kSpec.neutral));
  }
}

TEST_CASE("colour_for: non-finite values are neutral") {
  const Band band = spec_temperature_band();
  CHECK(same(colour_for(band, std::numeric_limits<double>::quiet_NaN()), kSpec.neutral));
  CHECK(same(colour_for(band, std::numeric_limits<double>::infinity()), kSpec.neutral));
  CHECK(same(colour_for(band, -std::numeric_limits<double>::infinity()), kSpec.neutral));
}

TEST_CASE("colour_for: uses the palette it is given") {
  const Band band = spec_temperature_band();
  const Palette mono{{100, 100, 100}, {0, 0, 0}, {255, 255, 255}};
  CHECK(same(colour_for(band, 22.0, mono), mono.neutral));
  CHECK(same(colour_for(band, 10.0, mono), mono.cold));
  CHECK(same(colour_for(band, 30.0, mono), mono.hot));
  CHECK(same(colour_for(band, 19.75, mono), Rgb888{50, 50, 50}));
}

TEST_CASE("colour_for: soil thirst runs blue when wet, red when dry (D-010)") {
  const Band soil = default_band(Layer::Soil);  // comfort 0.1..0.6 thirst, scale 0..1
  CHECK(same(colour_for(soil, 0.0), kSpec.cold));
  CHECK(same(colour_for(soil, 0.3), kSpec.neutral));
  CHECK(same(colour_for(soil, 1.0), kSpec.hot));
}

TEST_CASE("colour565_for packs colour_for's result") {
  const Band band = spec_temperature_band();
  for (double v = 10.0; v <= 35.0; v += 0.25) {
    CHECK(colour565_for(band, v) == to_rgb565(colour_for(band, v)));
  }
  CHECK(colour565_for(band, 22.0) == to_rgb565(Rgb888{30, 35, 41}));
  CHECK(colour565_for(band, 0.0) == to_rgb565(Rgb888{42, 120, 214}));
}

// ---------------------------------------------------------------------------------------------
// ColourLut
// ---------------------------------------------------------------------------------------------

TEST_CASE("build_colour_lut: entries sample colour565_for across the scale") {
  const Band band = spec_temperature_band();
  const ColourLut lut = build_colour_lut(band);
  REQUIRE(lut.entries.size() == kDefaultColourLutSize);
  CHECK(lut.lo == 18.0F);
  CHECK(lut.hi == 27.5F);
  CHECK(lut.entries.front() == to_rgb565(kSpec.cold));
  CHECK(lut.entries.back() == to_rgb565(kSpec.hot));
  for (std::size_t i = 0; i < lut.entries.size(); ++i) {
    const double v = 18.0 + 9.5 * static_cast<double>(i) / 255.0;
    CHECK(lut.entries[i] == colour565_for(band, v));
  }
}

TEST_CASE("build_colour_lut: a size below 2 is treated as 2") {
  const Band band = spec_temperature_band();
  for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
    const ColourLut lut = build_colour_lut(band, kSpec, size);
    REQUIRE(lut.entries.size() == 2U);
    CHECK(lut.entries[0] == to_rgb565(kSpec.cold));
    CHECK(lut.entries[1] == to_rgb565(kSpec.hot));
  }
}

TEST_CASE("ColourLut::lookup: nearest entry, clamped, non-finite and empty give 0") {
  const Band band = spec_temperature_band();
  const ColourLut lut = build_colour_lut(band);
  CHECK(lut.lookup(18.0F) == lut.entries.front());
  CHECK(lut.lookup(-1e30F) == lut.entries.front());
  CHECK(lut.lookup(27.5F) == lut.entries.back());
  CHECK(lut.lookup(1e30F) == lut.entries.back());
  CHECK(lut.lookup(22.5F) == to_rgb565(kSpec.neutral));
  // Round to nearest: a value 0.4 of a step above entry 10 is entry 10, 0.6 is entry 11.
  const float step = 9.5F / 255.0F;
  CHECK(lut.lookup(18.0F + 10.4F * step) == lut.entries[10]);
  CHECK(lut.lookup(18.0F + 10.6F * step) == lut.entries[11]);
  CHECK(lut.lookup(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(lut.lookup(std::numeric_limits<float>::infinity()) == 0);
  CHECK(ColourLut{}.lookup(20.0F) == 0);
}

TEST_CASE("ColourLut::lookup: within one 565 step per channel of colour565_for") {
  const Band band = spec_temperature_band();
  const ColourLut lut = build_colour_lut(band);
  int worst = 0;
  for (float v = 15.0F; v <= 30.0F; v += 0.01F) {
    const Rgb888 exact = from_rgb565(colour565_for(band, static_cast<double>(v)));
    const Rgb888 got = from_rgb565(lut.lookup(v));
    const int d = std::abs(exact.r - got.r) + std::abs(exact.g - got.g) + std::abs(exact.b - got.b);
    if (d > worst) worst = d;
  }
  CAPTURE(worst);
  CHECK(worst <= 24);  // three channels, at most one 565 step (8 levels) each
}

TEST_CASE("ColourLut: a degenerate scale saturates by side") {
  Band band = spec_temperature_band();
  band.scale_max = band.scale_min;
  const ColourLut lut = build_colour_lut(band, kSpec, 4);
  CHECK(lut.lookup(10.0F) == lut.entries.front());
  CHECK(lut.lookup(18.0F) == lut.entries.front());
  CHECK(lut.lookup(18.5F) == lut.entries.back());
}

TEST_CASE("build_colour_lut: the reuse form keeps its storage") {
  const Band band = spec_temperature_band();
  ColourLut lut;
  build_colour_lut(band, kSpec, 256, lut);
  const Rgb565* storage = lut.entries.data();
  build_colour_lut(default_band(Layer::Humidity), kSpec, 256, lut);
  CHECK(lut.entries.data() == storage);
  CHECK(lut.lo == 15.0F);
  CHECK(lut.entries.front() == colour565_for(default_band(Layer::Humidity), 15.0));
}
