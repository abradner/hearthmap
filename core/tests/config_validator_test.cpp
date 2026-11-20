// ConfigValidator: validate() one unit per step, with exactly validate()'s issues (D-038).
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/config.h"

using namespace hearthmap;

namespace {

std::vector<ConfigIssue> stepped(const Config& config, std::size_t* steps = nullptr) {
  ConfigValidator v(config);
  std::size_t n = 0;
  std::vector<ConfigIssue> all;
  while (true) {
    const bool done = v.step();
    ++n;
    for (auto& i : v.take_issues()) all.push_back(std::move(i));  // collected as they come
    if (done) break;
  }
  if (steps != nullptr) *steps = n;
  return all;
}

void check_same(const Config& config) {
  const std::vector<ConfigIssue> whole = validate(config);
  std::size_t steps = 0;
  const std::vector<ConfigIssue> parts = stepped(config, &steps);
  CHECK(steps == 3 + config.rooms.size() + config.pins.size());
  REQUIRE(parts.size() == whole.size());
  for (std::size_t i = 0; i < whole.size(); ++i) {
    CAPTURE(whole[i].path);
    CHECK(parts[i].severity == whole[i].severity);
    CHECK(parts[i].path == whole[i].path);
    CHECK(parts[i].message == whole[i].message);
  }
}

}  // namespace

TEST_CASE("stepping gives validate()'s issues, in order, for valid and broken configs") {
  const std::vector<std::function<void(Config&)>> mutations = {
      [](Config&) {},
      [](Config& c) { c.rooms[1].id = c.rooms[0].id; },
      [](Config& c) { c.pins[2].id = c.pins[0].id; },
      [](Config& c) { c.pins[1].room_id = "nowhere"; },
      [](Config& c) { c.rooms[0].polygon = {{0.1, 0.1}, {0.9, 0.9}, {0.9, 0.1}, {0.1, 0.9}}; },
      [](Config& c) { c.rooms[2].polygon[0].x = 1.5; },
      [](Config& c) { c.pins[3].position = {0.99, 0.99}; },  // a warning: outside its room
      [](Config& c) {
        c.temperature_unit = TemperatureUnit::Fahrenheit;
        c.version = 2;
        c.canvas.width = 0;
      },
      [](Config& c) { c.rooms.clear(); },
      [](Config& c) { c.pins.clear(); },
      [](Config& c) {
        while (c.pins.size() < 260) c.pins.push_back(c.pins[0]);
      },
      [](Config& c) {
        c.hud.max_items = 0;
        c.heat.grid_cols = 1;
      },
  };
  for (std::size_t m = 0; m < mutations.size(); ++m) {
    CAPTURE(m);
    Config c = test::flat_config();
    mutations[m](c);
    check_same(c);
  }
}

TEST_CASE("each room step reports only its own room, each pin step only its own pin") {
  Config c = test::flat_config();
  c.rooms[1].id = c.rooms[0].id;  // an issue on room 1
  c.pins[2].room_id = "nowhere";  // an issue on pin 2
  ConfigValidator v(c);
  v.step();  // version, canvas, room count
  for (std::size_t r = 0; r < c.rooms.size(); ++r) {
    v.step();
    for (const auto& i : v.take_issues()) {
      CHECK(i.path.rfind("/rooms/" + std::to_string(r) + "/", 0) == 0);
    }
  }
  v.step();  // pin count
  for (std::size_t p = 0; p < c.pins.size(); ++p) {
    v.step();
    for (const auto& i : v.take_issues()) {
      CHECK(i.path.rfind("/pins/" + std::to_string(p) + "/", 0) == 0);
    }
  }
  CHECK(v.step());  // layers and scalars: the last step
  CHECK(v.done());
  CHECK(v.step());  // stepping a finished validator is a no-op
}
