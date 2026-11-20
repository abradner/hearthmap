#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "app.h"
#include "headless.h"
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

}  // namespace

TEST_CASE("end to end: a tap on a plant opens its overlay; the scrim closes it") {
  const std::string dir = std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/";
  const Config config = *JsonConfigSource(slurp(dir + "config.json")).load().config;
  const ReadingsFixture fx = *parse_readings(slurp(dir + "readings/thirsty-many.json")).fixture;
  HeadlessDisplay display(1280, 800);
  App app(display.screen(), config, fx.readings, fx.now, 0, nullptr);
  CHECK(app.ui_state().active_layer == Layer::Temperature);
  CHECK(app.view_model().hud.items.size() == 4);
  CHECK(app.view_model().hud.overflow == 2);

  display.tap(580, 119);  // basil
  CHECK(app.process_events() == 1);
  REQUIRE(app.ui_state().overlay_pin.has_value());
  CHECK(*app.ui_state().overlay_pin == "plant_basil");
  CHECK_FALSE(lv_obj_is_hidden(app.screen().widgets().scrim));

  display.tap(100, 700);  // the scrim
  CHECK(app.process_events() == 1);
  CHECK_FALSE(app.ui_state().overlay_pin.has_value());
  CHECK(lv_obj_is_hidden(app.screen().widgets().scrim));

  app.set_layer(Layer::Soil);
  CHECK(app.view_model().active_layer == Layer::Soil);
  CHECK(app.heat_image_bytes() == 864U * 720U * 2U);

  // No events: nothing to do.
  CHECK(app.process_events() == 0);
}
