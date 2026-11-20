#include <doctest/doctest.h>

#include "hearthmap/ui/format.h"
#include "hearthmap/ui/theme.h"

using namespace hearthmap;
using namespace hearthmap::ui;

TEST_CASE("severity colours follow the spec") {
  CHECK(severity_colour(Severity::Ok) == 0x30A46C);
  CHECK(severity_colour(Severity::Warn) == 0xF5A524);
  CHECK(severity_colour(Severity::Danger) == 0xE5484D);
  CHECK(severity_colour(Severity::Stale) == 0x6B7280);
}

TEST_CASE("moisture percent is 1 - thirst, rounded and clamped") {
  CHECK(moisture_percent(1.0) == 0);
  CHECK(moisture_percent(0.0) == 100);
  CHECK(moisture_percent(0.955) == 5);  // 4.5 rounds away from zero
  CHECK(moisture_percent(0.646) == 35);
  CHECK(moisture_percent(1.5) == 0);
  CHECK(moisture_percent(-0.2) == 100);
}

TEST_CASE("ring length has a 3% floor and a full-turn ceiling") {
  CHECK(ring_permille(1.0) == 30);
  CHECK(ring_permille(0.99) == 30);
  CHECK(ring_permille(0.96) == 40);
  CHECK(ring_permille(0.0) == 1000);
  CHECK(ring_permille(-1.0) == 1000);
}

TEST_CASE("values carry their units") {
  CHECK(format_value(Role::Temperature, 22.64) == "22.6\xC2\xB0");
  CHECK(format_value(Role::Temperature, -3.0) == "-3.0\xC2\xB0");
  CHECK(format_value(Role::Humidity, 67.6) == "68%");
  CHECK(format_value(Role::Battery, 88.0) == "88%");
  CHECK(format_value(Role::Co2, 620.0) == "620 ppm");
  CHECK(format_value(Role::Pm25, 6.0) == "6 ug/m3");
  CHECK(format_value(Role::Voc, 110.0) == "110");
  CHECK(format_value(Role::Moisture, 0.96) == "4%");
  CHECK(format_value(Role::PredictedDryAt, 1.0).empty());
}

TEST_CASE("role labels say what a probe in a pot measures") {
  CHECK(role_label(Role::Temperature, PinKind::Plant) == "Soil temp");
  CHECK(role_label(Role::Temperature, PinKind::Soil) == "Soil temp");
  CHECK(role_label(Role::Temperature, PinKind::Temperature) == "Temperature");
  CHECK(role_label(Role::Humidity, PinKind::Plant) == "Air humidity");
  CHECK(role_label(Role::Humidity, PinKind::Humidity) == "Humidity");
  CHECK(role_label(Role::Co2, PinKind::AirQuality) == "CO2");
}

TEST_CASE("layer tab labels") {
  CHECK(layer_label(Layer::Temperature) == "Temp");
  CHECK(layer_label(Layer::Humidity) == "Humidity");
  CHECK(layer_label(Layer::Soil) == "Soil");
  CHECK(layer_label(Layer::Air) == "Air");
}

TEST_CASE("seen text steps from minutes to hours to days at its boundaries") {
  CHECK(format_seen(-30) == "seen just now");
  CHECK(format_seen(59) == "seen just now");
  CHECK(format_seen(60) == "seen 1 min ago");
  CHECK(format_seen(119 * 60 + 59) == "seen 119 min ago");
  CHECK(format_seen(120 * 60) == "seen 2 h ago");
  CHECK(format_seen(47 * 3600 + 3599) == "seen 47 h ago");
  CHECK(format_seen(48 * 3600) == "seen 2 d ago");
}

TEST_CASE("thirst verdicts") {
  Band band = default_band(Layer::Soil);  // comfort 0.1..0.6
  Assessment a;
  a.role = Role::Moisture;
  a.freshness = Freshness::Fresh;
  a.band = band;

  a.value = 0.96;
  a.severity = Severity::Danger;
  CHECK(thirst_verdict(a) == "4%  water now");

  a.value = 0.8;
  a.severity = Severity::Warn;
  CHECK(thirst_verdict(a) == "20%  water soon");

  a.value = 0.05;  // below comfort_min: the wet side
  a.severity = Severity::Warn;
  CHECK(thirst_verdict(a) == "95%  too wet");

  a.severity = Severity::Danger;
  CHECK(thirst_verdict(a) == "95%  too wet");

  a.value = 0.4;
  a.severity = Severity::Ok;
  CHECK(thirst_verdict(a) == "60%  happy");

  a.freshness = Freshness::TooOld;
  a.value.reset();
  a.severity = Severity::Stale;
  CHECK(thirst_verdict(a) == "no data");

  // Fresh but somehow Stale severity still reads as no data, never a percentage.
  a.freshness = Freshness::Fresh;
  a.value = 0.4;
  CHECK(thirst_verdict(a) == "no data");
}

TEST_CASE("summary text words core's HouseSummary, leaving out empty parts") {
  CHECK(summary_text(HouseSummary{21.0, 50.75, 6.0}) == "House 21.0\xC2\xB0   51% RH   PM2.5 6");
  CHECK(summary_text(HouseSummary{std::nullopt, 52.0, std::nullopt}) == "House 52% RH");
  CHECK(summary_text(HouseSummary{std::nullopt, std::nullopt, 12.0}) == "House PM2.5 12");
  CHECK(summary_text(HouseSummary{}) == "House: no data");
}
