#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "clock_text.h"
#include "hearthmap/ui/format.h"
#include "hearthmap/view_model.h"
#include "json_config.h"

using namespace hearthmap;
using namespace hearthmap::host;

// The house summary through the shared path the host and the device both use (D-049): core's
// house_summary() worded by ui_lvgl's summary_text(), on the example's JSON fixtures.
static std::string house_summary_text(const Config& c, const Readings& r, Timestamp now) {
  return ui::summary_text(house_summary(c, r, now));
}

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Flat {
  Config config;
  ReadingsFixture fixture;
};

Flat load(const char* readings) {
  const std::string dir = std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/";
  Flat f;
  f.config = *JsonConfigSource(slurp(dir + "config.json")).load().config;
  f.fixture = *parse_readings(slurp(dir + "readings/" + readings + ".json")).fixture;
  return f;
}

}  // namespace

TEST_CASE("house summary averages fresh indoor readings that feed their layer") {
  const Flat f = load("all-ok");
  // Temperatures feeding the layer indoors: 21.5, 22.0, 20.5, 20.0, 19.5 and the bathroom's
  // 22.5 (a humidity pin's temperature feeds; the fiddle's pot probe does not; the balcony is
  // outdoor) -> 21.0. Humidity: 48, 45, 52, 58 -> 50.75. PM2.5: 6.
  CHECK(house_summary_text(f.config, f.fixture.readings, f.fixture.now) ==
        "House 21.0\xC2\xB0   51% RH   PM2.5 6");
}

TEST_CASE("stale readings drop out of the summary") {
  const Flat f = load("stale-sensors");
  const std::string s = house_summary_text(f.config, f.fixture.readings, f.fixture.now);
  CHECK(s.rfind("House ", 0) == 0);
  // Kitchen and study temperatures are too old; the rest still average.
  CHECK(s.find("\xC2\xB0") != std::string::npos);
}

TEST_CASE("no fresh readings at all says so") {
  const Flat f = load("all-ok");
  CHECK(house_summary_text(f.config, Readings{}, f.fixture.now) == "House: no data");
  // Only PM2.5 present: no leading separator.
  Readings only_pm;
  only_pm.by_entity["sensor.example_living_pm25"] =
      Reading{ReadingState::Value, 12.0, "", f.fixture.now};
  CHECK(house_summary_text(f.config, only_pm, f.fixture.now) == "House PM2.5 12");
}

TEST_CASE("clock text in UTC with an offset, wrapping at midnight") {
  CHECK(clock_text(1768467600, 0) == "09:00");
  CHECK(clock_text(1768467600, 600) == "19:00");
  CHECK(clock_text(1768467600, -570) == "23:30");
  CHECK(clock_text(1768467600 + 15 * 3600 + 59, 0) == "00:00");
}
