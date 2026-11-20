// The cases in schema/semantic-cases.json, through core's validate(). The editor's tests read
// the same file (editor/test/semanticCases.test.ts), so its polite checks agree with this, the
// enforcement, case by case (docs/decisions.md D-040, D-048).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "hearthmap/config.h"
#include "semantic_cases.h"

using namespace hearthmap;

TEST_CASE("schema/semantic-cases.json: validate() agrees with every case") {
  const auto& cases = test::semantic_cases();
  REQUIRE(cases.size() >= 8);
  int valid = 0;
  int refused = 0;
  for (const test::SemanticCase& c : cases) {
    CAPTURE(std::string(c.name));
    Config config;
    config.canvas.width = test::kSemanticCanvasWidth;
    config.canvas.height = test::kSemanticCanvasHeight;
    config.canvas.orientation = Orientation::Landscape;
    config.canvas.map_area = c.map_area;
    Room room;
    room.id = "room";
    room.name = "Room";
    room.polygon = c.polygon;
    config.rooms.push_back(room);
    const std::vector<ConfigIssue> issues = validate(config);
    std::string errors;
    bool at_path = false;
    for (const ConfigIssue& i : issues) {
      if (i.severity != IssueSeverity::Error) continue;
      errors += i.path + ": " + i.message + "\n";
      if (i.path == c.path) at_path = true;
    }
    CAPTURE(errors);
    if (c.valid) {
      CHECK(errors.empty());
      ++valid;
    } else {
      CHECK(at_path);
      ++refused;
    }
  }
  CHECK(valid > 0);  // both outcomes are exercised, so neither can pass vacuously
  CHECK(refused > 0);
}
