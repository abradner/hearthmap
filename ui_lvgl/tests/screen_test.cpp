#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fixtures.h"
#include "headless.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/ui/screen.h"
#include "hearthmap/ui/theme.h"

using namespace hearthmap;
using namespace hearthmap::ui;

#if defined(__GNUC__)
#pragma GCC diagnostic push
// lv_image_header_t is a set of bit-fields; GCC's -Wconversion flags every write to them.
#pragma GCC diagnostic ignored "-Wconversion"
#endif

namespace {

struct RecordingSink : EventSink {
  std::vector<UiEvent> events;
  void on_ui_event(const UiEvent& e) override { events.push_back(e); }
};

struct OnePhoto : ImageProvider {
  std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(8 * 8 * 2, 0x55);
  lv_image_dsc_t dsc{};
  OnePhoto() {
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = 8;
    dsc.header.h = 8;
    dsc.header.stride = 16;
    dsc.data_size = 128;
    dsc.data = pixels.data();
  }
  const lv_image_dsc_t* plant_photo(std::string_view id) const override {
    return id == "fern" ? &dsc : nullptr;
  }
};

std::string text(lv_obj_t* label) {
  return std::string(lv_label_get_text(label));
}
bool hidden(lv_obj_t* o) {
  return lv_obj_is_hidden(o);
}
bool hidden_or_ancestor(lv_obj_t* o) {
  for (; o != nullptr; o = lv_obj_get_parent(o)) {
    if (hidden(o)) return true;
  }
  return false;
}
std::uint32_t text_rgb(lv_obj_t* l) {
  return lv_color_to_u32(lv_obj_get_style_text_color(l, LV_PART_MAIN)) & 0xFFFFFFU;
}
std::uint32_t bg_rgb(lv_obj_t* o) {
  return lv_color_to_u32(lv_obj_get_style_bg_color(o, LV_PART_MAIN)) & 0xFFFFFFU;
}

// Everything a test needs: display, config, sink, photos, heat buffer and a Screen.
struct Rig {
  host::HeadlessDisplay display{1280, 800};
  Config config = test::small_config();
  RecordingSink sink;
  OnePhoto photos;
  HeatRasterizer raster{config};
  std::vector<Rgb565> heat = std::vector<Rgb565>(raster.pixel_count(), 0);
  lv_image_dsc_t heat_dsc{};
  std::unique_ptr<Screen> screen;
  HeaderText header{"09:00", "House 21.4\xC2\xB0   52% RH"};

  Rig() {
    heat_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    heat_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    heat_dsc.header.w = static_cast<std::uint32_t>(raster.width());
    heat_dsc.header.h = static_cast<std::uint32_t>(raster.height());
    heat_dsc.header.stride = static_cast<std::uint32_t>(raster.width() * 2);
    heat_dsc.data_size = static_cast<std::uint32_t>(heat.size() * 2);
    heat_dsc.data = reinterpret_cast<const std::uint8_t*>(heat.data());
    ScreenDeps deps;
    deps.fonts = builtin_fonts();
    deps.sink = &sink;
    deps.photos = &photos;
    screen = std::make_unique<Screen>(display.screen(), config, &heat_dsc, deps);
  }
  const ScreenWidgets& w() const { return screen->widgets(); }
  void show(const ViewModel& vm, Timestamp now = 1000) { screen->update(vm, header, now); }
};

}  // namespace

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

TEST_CASE("the tree has one widget per room and pin, and the HUD pool is the config's cap") {
  Rig rig;
  rig.show(test::temperature_vm(rig.config));
  const auto& w = rig.w();
  CHECK(w.room_pulse.size() == 3);
  CHECK(w.room_label.size() == 3);
  CHECK(text(w.room_label[0]) == "West");
  CHECK(w.pin.size() == 5);
  CHECK(w.hud_card.size() == 2);
  CHECK(w.tabs.size() == 4);
  CHECK(text(w.clock) == "09:00");
  // Plain pins have a value label and no ring; plant and soil pins the reverse.
  CHECK(w.pin_value[0] != nullptr);
  CHECK(w.pin_ring[0] == nullptr);
  CHECK(w.pin_value[1] == nullptr);
  CHECK(w.pin_ring[1] != nullptr);
  CHECK(w.pin_ring[4] != nullptr);
}

TEST_CASE("plain pins show the active value, no data when stale, and dim off-layer") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  CHECK(text(w.pin_value[0]) == "22.6\xC2\xB0");
  CHECK(text_rgb(w.pin_value[0]) == theme::kTextPrimary);
  CHECK(lv_obj_get_style_opa(w.pin[0], LV_PART_MAIN) == LV_OPA_COVER);
  // The humidity pin does not feed the temperature layer: dimmed, no text.
  CHECK(text(w.pin_value[3]).empty());
  CHECK(hidden(w.pin_value[3]));  // no empty backing pill
  CHECK_FALSE(hidden(w.pin_value[0]));
  CHECK(lv_obj_get_style_opa(lv_obj_get_child(w.pin[3], 0), LV_PART_MAIN) == LV_OPA_40);  // the dot

  vm.pins[0].active_value.reset();
  vm.pins[0].active_severity = Severity::Stale;
  vm.pins[0].primary = test::stale(Role::Temperature, Freshness::TooOld);
  rig.show(vm);
  CHECK(text(w.pin_value[0]) == "no data");
  CHECK(text_rgb(w.pin_value[0]) == theme::kStale);
}

TEST_CASE("a warn value is coloured and badged; danger on the active layer pulses") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 28.0;
  vm.pins[0].active_severity = Severity::Warn;
  rig.show(vm);
  const auto& w = rig.w();
  CHECK(text_rgb(w.pin_value[0]) == theme::kWarn);
  CHECK_FALSE(hidden(w.pin_badge[0]));
  CHECK(bg_rgb(w.pin_badge[0]) == theme::kWarn);
  CHECK(hidden(w.pin_pulse[0]));
  // The dry fern is danger but off the temperature layer: badge yes, pulse no.
  CHECK_FALSE(hidden(w.pin_badge[1]));
  CHECK(bg_rgb(w.pin_badge[1]) == theme::kDanger);
  CHECK(hidden(w.pin_pulse[1]));
  // An ok pin has no badge.
  CHECK(hidden(w.pin_badge[4]));

  vm.pins[0].active_severity = Severity::Danger;
  vm.pins[0].active_value = 33.0;
  rig.show(vm);
  CHECK_FALSE(hidden(w.pin_pulse[0]));
  CHECK(bg_rgb(w.pin_badge[0]) == theme::kDanger);
}

TEST_CASE("plant rings show moisture in the status colour; stale plants lose the arc") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  CHECK(lv_arc_get_value(w.pin_ring[1]) == 50);  // thirst 0.95 -> 5%
  CHECK(lv_arc_get_value(w.pin_ring[2]) == 200);
  CHECK((lv_color_to_u32(lv_obj_get_style_arc_color(w.pin_ring[2], LV_PART_INDICATOR)) &
         0xFFFFFFU) == theme::kWarn);
  CHECK(lv_obj_get_style_arc_opa(w.pin_ring[2], LV_PART_INDICATOR) == LV_OPA_COVER);

  vm.pins[2].primary = test::stale(Role::Moisture, Freshness::Unavailable);
  vm.pins[2].active_severity = Severity::Stale;
  rig.show(vm);
  CHECK(lv_obj_get_style_arc_opa(w.pin_ring[2], LV_PART_INDICATOR) == LV_OPA_TRANSP);
  CHECK(hidden(w.pin_badge[2]));
}

TEST_CASE("plant photos come from the provider; a missing photo is the leaf") {
  Rig rig;
  rig.show(test::temperature_vm(rig.config));
  const auto& w = rig.w();
  // Disc holder -> face -> image.
  lv_obj_t* fern_face = lv_obj_get_child(lv_obj_get_child(w.pin[1], 0), 0);
  lv_obj_t* fern_img = lv_obj_get_child(fern_face, 0);
  CHECK(lv_image_get_src(fern_img) == &rig.photos.dsc);
  lv_obj_t* ivy_img = lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(w.pin[2], 0), 0), 0);
  const void* leaf = lv_image_get_src(ivy_img);
  CHECK(leaf != nullptr);
  CHECK(leaf != &rig.photos.dsc);
}

TEST_CASE("rooms pulse at warn or danger and never for ok or stale") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  for (auto* p : w.room_pulse) CHECK(hidden(p));
  vm.rooms[0].pulse = Severity::Warn;
  vm.rooms[1].pulse = Severity::Danger;
  vm.rooms[2].pulse = Severity::Stale;
  rig.show(vm);
  CHECK_FALSE(hidden(w.room_pulse[0]));
  CHECK((lv_color_to_u32(lv_obj_get_style_line_color(w.room_pulse[0], LV_PART_MAIN)) & 0xFFFFFFU) ==
        theme::kWarn);
  CHECK((lv_color_to_u32(lv_obj_get_style_line_color(w.room_pulse[1], LV_PART_MAIN)) & 0xFFFFFFU) ==
        theme::kDanger);
  CHECK(hidden(w.room_pulse[2]));
  vm.rooms[0].pulse = Severity::Ok;
  rig.show(vm);
  CHECK(hidden(w.room_pulse[0]));
}

TEST_CASE("the HUD lists items in view-model order, with a count and an overflow line") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.hud.overflow = 3;
  rig.show(vm);
  const auto& w = rig.w();
  CHECK(text(w.hud_count) == "5");
  CHECK_FALSE(hidden(w.hud_count));
  CHECK(hidden(w.hud_empty));
  CHECK(text(w.hud_name[0]) == "Fern");
  CHECK(text(w.hud_room[0]) == "West");
  CHECK(text(w.hud_percent[0]) == "5%");
  CHECK(text_rgb(w.hud_percent[0]) == theme::kDanger);
  CHECK(text(w.hud_name[1]) == "Ivy");
  CHECK(text(w.hud_percent[1]) == "20%");
  CHECK(text_rgb(w.hud_percent[1]) == theme::kWarn);
  CHECK_FALSE(hidden(w.hud_more));
  CHECK(text(w.hud_more) == "+3 more");

  vm.hud.items.pop_back();
  vm.hud.overflow = 0;
  rig.show(vm);
  CHECK(text(w.hud_count) == "1");
  CHECK_FALSE(hidden(w.hud_card[0]));
  CHECK(hidden(w.hud_card[1]));
  CHECK(hidden(w.hud_more));

  vm.hud.items.clear();
  rig.show(vm);
  CHECK(hidden(w.hud_count));
  CHECK_FALSE(hidden(w.hud_empty));
  CHECK(hidden(w.hud_card[0]));
}

TEST_CASE("tabs: one pill per enabled layer, the active one filled") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  CHECK(bg_rgb(w.tabs[0]) == theme::kTextPrimary);
  CHECK(bg_rgb(w.tabs[1]) == theme::kTabInactive);
  CHECK(text(lv_obj_get_child(w.tabs[2], 0)) == "Soil");
  vm.active_layer = Layer::Soil;
  rig.show(vm);
  CHECK(bg_rgb(w.tabs[0]) == theme::kTabInactive);
  CHECK(bg_rgb(w.tabs[2]) == theme::kTextPrimary);
  lv_obj_t* before = w.tabs[0];
  vm.tabs = {Layer::Temperature, Layer::Soil};
  rig.show(vm);
  CHECK(w.tabs.size() == 2);
  CHECK(text(lv_obj_get_child(w.tabs[1], 0)) == "Soil");
  CHECK(bg_rgb(w.tabs[1]) == theme::kTextPrimary);
  (void)before;
}

TEST_CASE("the plant overlay shows thirst, happy range, the three stats and seen time") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  OverlayView ov;
  ov.pin_index = 1;
  ov.rows.push_back(
      test::row(test::fresh(Role::Moisture, 0.96, Severity::Danger, default_band(Layer::Soil)), "%",
                1000 - 300));
  ov.rows.push_back(test::row(test::fresh(Role::Battery, 91.0, Severity::Ok), "%", 1000));
  vm.overlay = ov;
  rig.show(vm, 1000);
  const auto& w = rig.w();
  CHECK_FALSE(hidden(w.scrim));
  CHECK(text(w.overlay_title) == "Fern");
  CHECK(text(w.overlay_subtitle) == "West   seen 5 min ago");
  CHECK_FALSE(hidden(w.overlay_thirst));
  CHECK(text(w.overlay_verdict) == "4%  water now");
  CHECK(text_rgb(w.overlay_verdict) == theme::kDanger);
  CHECK(text(w.overlay_happy) == "happy 40 to 90%");
  CHECK(text(w.overlay_stat_label[0]) == "Soil temp");
  CHECK(text(w.overlay_stat_value[0]) == "none");
  CHECK(text(w.overlay_stat_label[1]) == "Air humidity");
  CHECK(text(w.overlay_stat_label[2]) == "Battery");
  CHECK(text(w.overlay_stat_value[2]) == "91%");
  CHECK(text(w.overlay_stat_label[3]).empty());
  CHECK(text(w.overlay_history_title) == "Soil moisture, 7 days");

  // Time passes: only the subtitle changes.
  const std::size_t before = rig.screen->writes();
  rig.show(vm, 1000 + 600);
  CHECK(text(w.overlay_subtitle) == "West   seen 15 min ago");
  CHECK(rig.screen->writes() == before + 1);  // the subtitle and nothing else

  vm.overlay.reset();
  rig.show(vm);
  CHECK(hidden(w.scrim));
}

TEST_CASE("a non-plant overlay has no thirst row and lists its rows as stats") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  OverlayView ov;
  ov.pin_index = 0;
  ov.rows.push_back(test::row(test::fresh(Role::Temperature, 21.5, Severity::Ok), "C", 900));
  ov.rows.push_back(test::row(test::stale(Role::Humidity, Freshness::Unavailable), "%", 900));
  vm.overlay = ov;
  rig.show(vm, 1000);
  const auto& w = rig.w();
  CHECK(hidden(w.overlay_thirst));
  CHECK(text(w.overlay_subtitle) == "West   seen 1 min ago");
  CHECK(text(w.overlay_stat_label[0]) == "Temperature");
  CHECK(text(w.overlay_stat_value[0]) == "21.5\xC2\xB0");
  CHECK(text(w.overlay_stat_label[1]) == "Humidity");
  CHECK(text(w.overlay_stat_value[1]) == "no data");
  CHECK(text(w.overlay_stat_label[2]).empty());
  CHECK(text(w.overlay_history_title) == "Temperature, 7 days");

  // A pin never seen says so.
  ov.rows[0] = test::row(test::stale(Role::Temperature), "", std::nullopt);
  vm.overlay = ov;
  rig.show(vm, 1000);
  CHECK(text(w.overlay_subtitle) == "West   never seen");
}

TEST_CASE("update diffs: an identical view model writes nothing, a change writes only itself") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  const std::size_t after_first = rig.screen->writes();
  CHECK(after_first > 0);
  std::vector<lv_obj_t*> pins_before = w.pin;
  std::vector<lv_obj_t*> tabs_before = w.tabs;

  rig.show(vm);
  CHECK(rig.screen->writes() == after_first);

  vm.pins[0].active_value = 22.7;
  vm.pins[0].primary.value = 22.7;
  rig.show(vm);
  CHECK(rig.screen->writes() == after_first + 1);  // the one label
  CHECK(text(w.pin_value[0]) == "22.7\xC2\xB0");
  CHECK(w.pin == pins_before);
  CHECK(w.tabs == tabs_before);

  // A change that does not alter what is shown (a value change below display precision).
  vm.pins[0].active_value = 22.71;
  rig.show(vm);
  CHECK(rig.screen->writes() == after_first + 1);
}

TEST_CASE("taps become core events through LVGL's input pipeline") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  const auto& w = rig.w();
  rig.display.render();

  SUBCASE("a tap in the map is TapMap at the tap point, even on a pin") {
    rig.display.tap(256, 256);  // the west pin's centre: pins are not click targets
    REQUIRE(rig.sink.events.size() == 1);
    CHECK(rig.sink.events[0].type == UiEvent::Type::TapMap);
    CHECK(rig.sink.events[0].point.x == doctest::Approx(256));
    CHECK(rig.sink.events[0].point.y == doctest::Approx(256));
  }
  SUBCASE("a tab is SelectLayer") {
    lv_area_t a;
    lv_obj_get_coords(w.tabs[1], &a);
    rig.display.tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    REQUIRE(rig.sink.events.size() == 1);
    CHECK(rig.sink.events[0].type == UiEvent::Type::SelectLayer);
    CHECK(rig.sink.events[0].layer == Layer::Humidity);
  }
  SUBCASE("a HUD card is TapHudItem with its pin id") {
    lv_area_t a;
    lv_obj_get_coords(w.hud_card[1], &a);
    rig.display.tap(a.x1 + 100, (a.y1 + a.y2) / 2);
    REQUIRE(rig.sink.events.size() == 1);
    CHECK(rig.sink.events[0].type == UiEvent::Type::TapHudItem);
    CHECK(rig.sink.events[0].pin_id == "plant_ivy");
  }
  SUBCASE("the top bar outside the tabs sends nothing") {
    rig.display.tap(400, 30);
    CHECK(rig.sink.events.empty());
  }
  SUBCASE("with the overlay open: scrim closes, the card swallows, the map is unreachable") {
    OverlayView ov;
    ov.pin_index = 1;
    vm.overlay = ov;
    rig.show(vm);
    rig.display.render();
    CHECK_FALSE(hidden_or_ancestor(w.overlay_card));
    rig.display.tap(640, 400);  // on the card
    CHECK(rig.sink.events.empty());
    rig.display.tap(100, 700);  // on the scrim, over the map
    REQUIRE(rig.sink.events.size() == 1);
    CHECK(rig.sink.events[0].type == UiEvent::Type::CloseOverlay);
    rig.display.tap(1100, 300);  // on the scrim, over the HUD
    REQUIRE(rig.sink.events.size() == 2);
    CHECK(rig.sink.events[1].type == UiEvent::Type::CloseOverlay);
  }
}

TEST_CASE("the heat image is drawn at the map area") {
  Rig rig;
  for (auto& p : rig.heat) p = 0xF800;  // pure red
  rig.show(test::temperature_vm(rig.config));
  rig.screen->invalidate_heat();
  rig.display.render();
  const auto& fb = rig.display.framebuffer();
  auto px = [&](int x, int y) {
    return fb[static_cast<std::size_t>(y) * 1280U + static_cast<std::size_t>(x)];
  };
  CHECK(px(41 + 300, 77 + 300) == 0xF800);  // inside the map, away from pins and labels
  CHECK(px(20, 400) == to_rgb565(Rgb888{0x0D, 0x10, 0x13}));  // left margin: background
  CHECK(px(1000, 790) != 0xF800);                             // HUD column: not the map
}

TEST_CASE("pin labels and badges are drawn even though they sit outside their pin's box") {
  // Regression: LVGL 9.6 clips children to the parent's extended draw area, so a value label
  // right of its dot, and a badge past the disc's edge, vanished from the rendered frame while
  // every widget property still read correctly.
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 28.0;
  vm.pins[0].active_severity = Severity::Warn;
  rig.show(vm);
  rig.display.render();
  const auto& fb = rig.display.framebuffer();
  auto count = [&](lv_obj_t* obj, Rgb565 colour) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int n = 0;
    for (int y = a.y1; y <= a.y2; ++y) {
      for (int x = a.x1; x <= a.x2; ++x) {
        n += fb[static_cast<std::size_t>(y) * 1280U + static_cast<std::size_t>(x)] == colour;
      }
    }
    return n;
  };
  const Rgb565 warn = to_rgb565(Rgb888{0xF5, 0xA5, 0x24});
  const Rgb565 danger = to_rgb565(Rgb888{0xE5, 0x48, 0x4D});
  CHECK(count(rig.w().pin_value[0], warn) > 20);   // "28.0°" in warn colour
  CHECK(count(rig.w().pin_badge[0], warn) > 100);  // the triangle
  CHECK(count(rig.w().pin_badge[1], danger) > 100);
}

// ---- layout on dense configs -----------------------------------------------------------------

namespace {

struct DenseRig {
  host::HeadlessDisplay display{1280, 800};
  Config config = test::dense_config();
  RecordingSink sink;
  std::unique_ptr<Screen> screen;
  DenseRig() {
    ScreenDeps deps;
    deps.fonts = builtin_fonts();
    deps.sink = &sink;
    screen = std::make_unique<Screen>(display.screen(), config, nullptr, deps);
    screen->update(test::dense_vm(config), HeaderText{"09:00", "House"}, 1000);
    display.render();
  }
  const ScreenWidgets& w() const { return screen->widgets(); }
};

lv_area_t area_of(lv_obj_t* o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return a;
}

bool overlap(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

}  // namespace

TEST_CASE("long plant names stay on one line, truncated before the percentage") {
  DenseRig rig;
  const auto& w = rig.w();
  const int line = lv_font_get_line_height(builtin_fonts().px22);
  for (std::size_t c = 0; c < w.hud_card.size(); ++c) {
    const lv_area_t name = area_of(w.hud_name[c]);
    const lv_area_t room = area_of(w.hud_room[c]);
    const lv_area_t pct = area_of(w.hud_percent[c]);
    CHECK_MESSAGE(name.y2 - name.y1 + 1 <= line, "card " << c);
    CHECK(name.y2 < room.y1);  // never drawn over the room line
    CHECK(name.x2 < pct.x1);   // stops before the percentage column
    CHECK(room.x2 < pct.x1);
  }
  // LVGL's DOTS mode rewrites the shown text: the long name ends in an ellipsis.
  const std::string shown = text(w.hud_name[2]);
  CHECK(shown.rfind("Variegated", 0) == 0);
  CHECK(shown.size() >= 3);
  CHECK(shown.substr(shown.size() - 3) == "...");
}

TEST_CASE("a long name in the overlay header stays on one line") {
  DenseRig rig;
  ViewModel vm = test::dense_vm(rig.config);
  OverlayView ov;
  ov.pin_index = 2;
  vm.overlay = ov;
  rig.screen->update(vm, HeaderText{"09:00", "House"}, 1000);
  rig.display.render();
  const int line = lv_font_get_line_height(builtin_fonts().px34);
  const lv_area_t title = area_of(rig.w().overlay_title);
  CHECK(title.y2 - title.y1 + 1 <= line);
  CHECK(title.y2 < area_of(rig.w().overlay_subtitle).y1);
}

TEST_CASE("room labels fit their room, or are dropped when too narrow") {
  DenseRig rig;
  const auto& w = rig.w();
  const CanvasRect& m = rig.config.canvas.map_area;
  auto room_right = [&](std::size_t r) {
    double x = 0;
    for (const auto& v : rig.config.rooms[r].polygon) x = std::max(x, v.x);
    return m.x + x * m.width;
  };
  // Toilet (60 px) and Laundry: both shown, neither past its room, not touching each other.
  CHECK_FALSE(hidden(w.room_label[0]));
  CHECK(area_of(w.room_label[0]).x2 < room_right(0));
  CHECK_FALSE(overlap(area_of(w.room_label[0]), area_of(w.room_label[1])));
  CHECK(area_of(w.room_label[1]).x2 < room_right(1));
  // Duct (30 px): fewer than three characters fit, so no label at all.
  CHECK(hidden(w.room_label[2]));
}

TEST_CASE("a room label moves off a pin or value pill that covers its corner") {
  DenseRig rig;
  const auto& w = rig.w();
  // Lounge: plant 0 sits on its top-left corner.
  CHECK_FALSE(overlap(area_of(w.room_label[3]), area_of(w.pin[0])));
  CHECK_FALSE(overlap(area_of(w.room_label[3]), area_of(w.pin_badge[0])));
  // Laundry: the temperature pin's value pill sits where the label would go.
  CHECK_FALSE(overlap(area_of(w.room_label[1]), area_of(w.pin_value[7])));
  CHECK_FALSE(overlap(area_of(w.room_label[1]), area_of(w.pin[7])));
  // A label with nothing in the way stays at the top-left (Toilet).
  const lv_area_t toilet = area_of(w.room_label[0]);
  CHECK(toilet.y1 < 76 + 40);
}

TEST_CASE("seven HUD cards and the overflow line fit on the screen") {
  DenseRig rig;
  const auto& w = rig.w();
  REQUIRE(w.hud_card.size() == 7);
  const lv_area_t last = area_of(w.hud_card[6]);
  const lv_area_t more = area_of(w.hud_more);
  CHECK(text(w.hud_more) == "+3 more");
  CHECK(last.y2 < more.y1);
  CHECK(more.y2 <= 800 - 12);
  CHECK(last.y2 - last.y1 + 1 == theme::kHudCardHeight);  // the cards keep their size
}

TEST_CASE("an off-layer plain pin still looks stale when its sensor is stale") {
  // Review finding: off the active layer the stale dot went back to white.
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[3].primary = test::stale(Role::Humidity, Freshness::Unavailable);
  vm.pins[3].active_severity = Severity::Stale;  // off-layer: primary's severity
  rig.show(vm);
  lv_obj_t* dot = lv_obj_get_child(rig.w().pin[3], 0);
  CHECK(bg_rgb(dot) == theme::kStale);
  CHECK(hidden(rig.w().pin_value[3]));  // still no value text off-layer
  // And a fresh off-layer pin keeps the normal dot.
  lv_obj_t* fresh_dot = lv_obj_get_child(rig.w().pin[0], 0);
  CHECK(bg_rgb(fresh_dot) == theme::kTextPrimary);
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"  // lv_image_header_t bit-fields
#endif
namespace {

// A provider that hands out a new descriptor on every call and poisons the one it returned
// before for the same id, as the old contract allowed.
struct PoisoningPhotos : ImageProvider {
  mutable std::map<std::string, int, std::less<>> calls;
  mutable std::vector<std::unique_ptr<lv_image_dsc_t>> all;
  mutable std::map<std::string, lv_image_dsc_t*, std::less<>> last;
  std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(8 * 8 * 2, 0x33);
  const lv_image_dsc_t* plant_photo(std::string_view id) const override {
    ++calls[std::string(id)];
    auto it = last.find(id);
    if (it != last.end()) {
      it->second->data = nullptr;  // poison
      it->second->header.magic = 0;
    }
    auto d = std::make_unique<lv_image_dsc_t>();
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = 8;
    d->header.h = 8;
    d->header.stride = 16;
    d->data_size = 128;
    d->data = pixels.data();
    last[std::string(id)] = d.get();
    all.push_back(std::move(d));
    return all.back().get();
  }
};

}  // namespace
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

TEST_CASE("each photo id is requested once and every widget keeps a valid descriptor") {
  host::HeadlessDisplay display(1280, 800);
  Config config = test::small_config();
  RecordingSink sink;
  PoisoningPhotos photos;
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  deps.photos = &photos;
  Screen screen(display.screen(), config, nullptr, deps);
  ViewModel vm = test::temperature_vm(config);  // the fern (photo "fern") is in the HUD
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
  OverlayView ov;
  ov.pin_index = 1;  // the fern's overlay
  vm.overlay = ov;
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
  vm.hud.items = {HudItem{2, 0.8, Severity::Warn}, HudItem{1, 0.95, Severity::Danger}};
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
  CHECK(photos.calls["fern"] == 1);
  // Every image showing the fern points at a live (unpoisoned) descriptor.
  const auto& w = screen.widgets();
  lv_obj_t* map_img = lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(w.pin[1], 0), 0), 0);
  const auto* src = static_cast<const lv_image_dsc_t*>(lv_image_get_src(map_img));
  REQUIRE(src != nullptr);
  CHECK(src->data != nullptr);
  CHECK(src->header.magic == LV_IMAGE_HEADER_MAGIC);
  display.render();  // drawing must not touch a poisoned descriptor
}

TEST_CASE("map taps are reported in canvas pixels when the Screen's parent is offset") {
  host::HeadlessDisplay display(1280, 800);
  lv_obj_t* holder = lv_obj_create(display.screen());
  lv_obj_remove_style_all(holder);
  lv_obj_set_pos(holder, 100, 50);
  lv_obj_set_size(holder, 1280, 800);
  Config config = test::small_config();
  RecordingSink sink;
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  Screen screen(holder, config, nullptr, deps);
  screen.update(test::temperature_vm(config), HeaderText{"09:00", "House"}, 1000);
  display.render();
  display.tap(100 + 300, 50 + 300);  // canvas (300, 300), inside the map
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events[0].type == UiEvent::Type::TapMap);
  CHECK(sink.events[0].point.x == doctest::Approx(300));
  CHECK(sink.events[0].point.y == doctest::Approx(300));
}

TEST_CASE("a short room name that fits whole is shown even where an ellipsis would not fit") {
  host::HeadlessDisplay display(1280, 800);
  auto width = [](const char* t) {
    lv_point_t sz;
    lv_text_get_size(&sz, t, builtin_fonts().px20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
  };
  const std::int32_t full = width("IIII");
  REQUIRE(width("III...") > full);  // the case: whole name fits, the ellipsis form would not
  Config c = test::dense_config();
  // Toilet becomes exactly wide enough for "IIII" with the standard 12 + 8 px insets.
  const double x1 = static_cast<double>(full + 20 + 1) / 864.0;
  c.rooms[0].name = "IIII";
  c.rooms[0].polygon = {{0.0, 0.0}, {x1, 0.0}, {x1, 0.5}, {0.0, 0.5}};
  c.rooms[1].polygon[0].x = x1;
  c.rooms[1].polygon[3].x = x1;
  RecordingSink sink;
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  Screen screen(display.screen(), c, nullptr, deps);
  CHECK_FALSE(hidden(screen.widgets().room_label[0]));
}

TEST_CASE("a HUD cap too large for the screen shows the cards that fit and counts the rest") {
  host::HeadlessDisplay display(1280, 800);
  Config c = test::dense_config();
  c.hud.max_items = 9;
  RecordingSink sink;
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  Screen screen(display.screen(), c, nullptr, deps);
  ViewModel vm = test::dense_vm(c);
  vm.hud.items.push_back(HudItem{0, 0.9, Severity::Danger});
  vm.hud.items.push_back(HudItem{1, 0.9, Severity::Danger});  // 9 items
  vm.hud.overflow = 1;
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
  display.render();
  const auto& w = screen.widgets();
  const std::size_t shown = w.hud_card.size();
  CHECK(shown < 9);
  CHECK(area_of(w.hud_card[shown - 1]).y2 < 800);
  CHECK(area_of(w.hud_more).y2 <= 800 - 12);
  CHECK(text(w.hud_more) == "+" + std::to_string(1 + (9 - shown)) + " more");
  CHECK(text(w.hud_count) == "10");  // the title still counts every thirsty plant
}

TEST_CASE("dimming an off-layer pin leaves its badge at full strength") {
  Rig rig;
  rig.show(test::temperature_vm(rig.config));
  // The fern is off the temperature layer and in danger: its badge and every ancestor of the
  // badge must be fully opaque; the disc itself is dimmed.
  for (lv_obj_t* o = rig.w().pin_badge[1]; o != nullptr; o = lv_obj_get_parent(o)) {
    CHECK(lv_obj_get_style_opa(o, LV_PART_MAIN) == LV_OPA_COVER);
  }
  CHECK(lv_obj_get_style_opa(lv_obj_get_child(rig.w().pin[1], 0), LV_PART_MAIN) == LV_OPA_40);
}

TEST_CASE("with the soil layer disabled the overlay shows moisture without judging it") {
  Rig rig;
  rig.config.layers[index_of(Layer::Soil)].enabled = false;
  ViewModel vm = test::temperature_vm(rig.config);
  OverlayView ov;
  ov.pin_index = 1;
  // Core does not judge roles feeding a disabled layer: severity Ok, value still present.
  ov.rows.push_back(test::row(
      test::fresh(Role::Moisture, 0.95, Severity::Ok, default_band(Layer::Soil)), "%", 900));
  vm.overlay = ov;
  rig.show(vm);
  CHECK(text(rig.w().overlay_verdict) == "5%");
  CHECK(text_rgb(rig.w().overlay_verdict) == theme::kTextSecondary);
}

TEST_CASE("deleting the Screen's parent first leaves a Screen that is safe to update and destroy") {
  host::HeadlessDisplay display(1280, 800);
  lv_obj_t* holder = lv_obj_create(display.screen());
  Config config = test::small_config();
  RecordingSink sink;
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  auto screen = std::make_unique<Screen>(holder, config, nullptr, deps);
  screen->update(test::temperature_vm(config), HeaderText{"09:00", "House"}, 1000);
  lv_obj_delete(holder);  // LVGL frees the Screen's tree with its parent
  CHECK(screen->widgets().root == nullptr);
  const std::size_t before = screen->writes();
  screen->update(test::temperature_vm(config), HeaderText{"09:01", "House"}, 1060);
  CHECK(screen->writes() == before);  // nothing touched
  screen.reset();                     // must not delete the freed root again
}
