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

TEST_CASE("Palette defaults are the UI spec's heatmap colours (D-018)") {
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

TEST_CASE("colour_for: a side with no gradient room stays neutral (R9)") {
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
// ColourTable
// ---------------------------------------------------------------------------------------------

TEST_CASE("make_colour_table: entries sample colour565_for across the scale") {
  const Band band = spec_temperature_band();
  const ColourTable table = make_colour_table(band);
  CHECK(table.entries.front() == to_rgb565(kSpec.cold));
  CHECK(table.entries.back() == to_rgb565(kSpec.hot));
  CHECK(table.neutral == to_rgb565(kSpec.neutral));
  for (std::size_t i = 0; i < ColourTable::kEntries; ++i) {
    const double v = 18.0 + static_cast<double>(i) * 9.5 / 255.0;
    CHECK(table.entries[i] == colour565_for(band, v));
  }
}

TEST_CASE("ColourTable::lookup: exact comfort band, clamped ends, non-finite neutral") {
  const Band band = spec_temperature_band();
  const ColourTable table = make_colour_table(band);
  const Rgb565 neutral = to_rgb565(kSpec.neutral);
  CHECK(table.lookup(21.5F) == neutral);
  CHECK(table.lookup(23.5F) == neutral);
  CHECK(table.lookup(22.0F) == neutral);
  CHECK(table.lookup(18.0F) == to_rgb565(kSpec.cold));
  CHECK(table.lookup(-1e30F) == to_rgb565(kSpec.cold));
  CHECK(table.lookup(27.5F) == to_rgb565(kSpec.hot));
  CHECK(table.lookup(1e30F) == to_rgb565(kSpec.hot));
  CHECK(table.lookup(std::numeric_limits<float>::quiet_NaN()) == neutral);
  CHECK(table.lookup(std::numeric_limits<float>::infinity()) == neutral);
  CHECK(table.lookup(-std::numeric_limits<float>::infinity()) == neutral);
}

TEST_CASE("ColourTable::lookup: within half a step of colour_for everywhere") {
  // One table step moves each 8-bit channel by at most |far - neutral| / 255 * (span ratio);
  // check the looked-up colour is never further from the exact one than the colours of the two
  // neighbouring entries are from each other, plus one for 565 truncation.
  const Band band = spec_temperature_band();
  const ColourTable table = make_colour_table(band);
  int worst = 0;
  for (float v = 15.0F; v <= 30.0F; v += 0.01F) {
    const Rgb888 exact = from_rgb565(colour565_for(band, static_cast<double>(v)));
    const Rgb888 got = from_rgb565(table.lookup(v));
    const int d = std::abs(exact.r - got.r) + std::abs(exact.g - got.g) + std::abs(exact.b - got.b);
    if (d > worst) worst = d;
  }
  CAPTURE(worst);
  CHECK(worst <= 24);  // three channels, at most one 565 step (8 levels) each
}

TEST_CASE("ColourTable::lookup: degenerate scale sides stay neutral like colour_for (R9)") {
  Band band = spec_temperature_band();
  band.scale_min = band.comfort_min;
  const ColourTable table = make_colour_table(band);
  CHECK(table.lookup(10.0F) == to_rgb565(kSpec.neutral));
  CHECK(table.lookup(27.5F) == to_rgb565(kSpec.hot));
}

TEST_CASE("ColourTable::lookup: the comfort band is not quantised into a coloured entry") {
  // Entries are 1.0 apart; comfort_max = 254.6 lies 0.4 below the last entry, which is full
  // hot. Rounding to the nearest entry alone would paint a comfortable value hot.
  Band band;
  band.scale_min = 0.0;
  band.comfort_min = 0.0;
  band.comfort_max = 254.6;
  band.scale_max = 255.0;
  const ColourTable table = make_colour_table(band);
  REQUIRE(table.entries.back() == to_rgb565(kSpec.hot));
  CHECK(table.lookup(254.6F) == to_rgb565(kSpec.neutral));
  CHECK(table.lookup(254.9F) == to_rgb565(kSpec.hot));
}
