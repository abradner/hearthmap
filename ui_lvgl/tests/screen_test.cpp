#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "fixtures.h"
#include "headless.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/ui/memory.h"
#include "hearthmap/ui/screen.h"
#include "hearthmap/ui/theme.h"
#include "src/core/lv_refr_private.h"        // lv_refr_get_top_obj(), for the cover test
#include "src/display/lv_display_private.h"  // invalidated areas, for the fallback test

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

  std::size_t pulse_memory = 0;
  std::uint32_t pulse_min_divisor = 1;

  Rig() { init(); }
  explicit Rig(Config c) : config(std::move(c)) { init(); }
  Rig(Config c, std::size_t memory, std::uint32_t min_divisor)
      : config(std::move(c)), pulse_memory(memory), pulse_min_divisor(min_divisor) {
    init();
  }
  void init() {
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
    deps.pulse_memory_bytes = pulse_memory;
    deps.pulse_min_divisor = pulse_min_divisor;
    screen = std::make_unique<Screen>(display.screen(), config, &heat_dsc, deps);
  }
  const ScreenWidgets& w() const { return screen->widgets(); }
  void show(const ViewModel& vm, Timestamp now = 1000) {
    screen->update(vm, header, now);
    while (screen->prepare_step()) {
    }
  }
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

TEST_CASE("re-stamped readings show no seen time; source timestamps keep it") {
  // On the panel every value is re-stamped while Home Assistant is connected (D-012), so its
  // last_updated is "now" and "seen just now" would be a claim nobody checked (D-046).
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  OverlayView ov;
  ov.pin_index = 1;
  ov.rows.push_back(test::row(
      test::fresh(Role::Moisture, 0.96, Severity::Danger, default_band(Layer::Soil)), "%", 1000));
  vm.overlay = ov;
  SourceStatus restamped;
  restamped.restamped = true;
  rig.screen->update(vm, rig.header, 1000, restamped);
  CHECK(text(rig.w().overlay_subtitle) == "West");
  // Disconnected: nothing is re-stamped, so the age is the honest "last confirmed".
  SourceStatus offline;
  offline.offline = true;
  rig.screen->update(vm, rig.header, 1000 + 300, offline);
  CHECK(text(rig.w().overlay_subtitle) == "West   seen 5 min ago");
  // Positive control: the host's default status shows the seen time too.
  rig.show(vm, 1000 + 600);
  CHECK(text(rig.w().overlay_subtitle) == "West   seen 10 min ago");
}

TEST_CASE("offline replaces the house summary in the warn colour and clears on reconnect") {
  Rig rig;
  const ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  CHECK(text(rig.w().summary) == rig.header.summary);
  CHECK(text_rgb(rig.w().summary) == theme::kTextSecondary);
  SourceStatus offline;
  offline.offline = true;
  rig.screen->update(vm, rig.header, 1000, offline);
  CHECK(text(rig.w().summary) == "Offline");
  CHECK(text_rgb(rig.w().summary) == theme::kWarn);
  // A new summary while still offline does not replace the warning.
  rig.screen->update(vm, HeaderText{"09:00", "House 30.0\xC2\xB0"}, 1000, offline);
  CHECK(text(rig.w().summary) == "Offline");
  rig.show(vm);
  CHECK(text(rig.w().summary) == rig.header.summary);
  CHECK(text_rgb(rig.w().summary) == theme::kTextSecondary);
}

TEST_CASE("a heat image behind the active layer is veiled, then revealed in bands") {
  Rig rig;
  for (auto& p : rig.heat) p = 0xF800;  // pure red
  const ViewModel vm = test::temperature_vm(rig.config);
  // Map area 864 x 720 at (40, 76): one pixel near the top of the map, one near the bottom.
  auto px_at = [&](int map_y) {
    rig.display.render();
    return rig.display.framebuffer()[static_cast<std::size_t>(76 + map_y) * 1280U + 41U + 300U];
  };
  rig.show(vm);
  CHECK(px_at(5) == 0xF800);
  SourceStatus behind;
  behind.heat_behind = true;
  rig.screen->update(vm, rig.header, 1000, behind);
  // No colour of the image shows while it is behind: the map is the background.
  const std::uint16_t bg =
      static_cast<std::uint16_t>(lv_color_to_u16(lv_color_hex(theme::kBackground)));
  CHECK(px_at(5) == bg);
  CHECK(px_at(700) == bg);
  const std::size_t before = rig.screen->writes();
  rig.screen->update(vm, rig.header, 1000, behind);
  CHECK(rig.screen->writes() == before);  // unchanged status writes nothing

  // Caught up: the image comes back from the top, a strip per timer tick, not all at once.
  rig.show(vm);
  CHECK(px_at(700) == bg);
  rig.display.set_time_ms(2000);
  rig.display.run_timers();
  rig.display.set_time_ms(2040);
  rig.display.run_timers();
  CHECK(px_at(5) == 0xF800);
  CHECK(px_at(700) == bg);
  for (std::uint32_t t = 2040; t <= 3000; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
  }
  CHECK(px_at(5) == 0xF800);
  CHECK(px_at(700) == 0xF800);

  // Falling behind again mid-reveal veils everything again.
  rig.screen->update(vm, rig.header, 1000, behind);
  rig.show(vm);
  rig.display.set_time_ms(3040);
  rig.display.run_timers();
  rig.screen->update(vm, rig.header, 1000, behind);
  CHECK(px_at(5) == bg);
  CHECK(px_at(700) == bg);
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
  ViewModel vm = test::temperature_vm(config);
  vm.pins[1].on_active_layer = true;  // drawn at full strength, so the pixel is the photo's own
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
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

// --- Photos the way the device supplies them (docs/decisions.md D-039) ----------------------
//
// The panel's LVGL is built without RGB565A8, and LVGL's software transform of an RGB565 image
// needs it: a 128 px RGB565 photo scaled down to a 44 px pin disc logs "Color format 0x12 is not
// enabled" and draws nothing. The host's lv_conf.h now has the device's format set, so these
// tests see what the panel sees.

namespace {

int g_not_enabled = 0;
void count_lvgl_warnings(lv_log_level_t level, const char* buf) {
  if (level >= LV_LOG_LEVEL_WARN && std::string(buf).find("is not enabled") != std::string::npos) {
    ++g_not_enabled;
  }
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
// RGB565 photos, filled with one colour, at the overlay size and (optionally) every disc size.
struct DevicePhotos : ImageProvider {
  struct Img {
    std::vector<std::uint16_t> px;
    lv_image_dsc_t dsc{};
  };
  std::map<int, Img> by_size;
  bool sized = true;
  static constexpr std::uint16_t kColour = 0x07E0;  // pure green
  explicit DevicePhotos(bool with_sizes) : sized(with_sizes) {
    for (int s : {44, 52, 128}) {
      Img& i = by_size[s];
      i.px.assign(static_cast<std::size_t>(s * s), kColour);
      i.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      i.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      i.dsc.header.w = static_cast<std::uint32_t>(s);
      i.dsc.header.h = static_cast<std::uint32_t>(s);
      i.dsc.header.stride = static_cast<std::uint32_t>(s * 2);
      i.dsc.data_size = static_cast<std::uint32_t>(s * s * 2);
      i.dsc.data = reinterpret_cast<const std::uint8_t*>(i.px.data());
    }
  }
  const lv_image_dsc_t* plant_photo(std::string_view id) const override {
    return id == "fern" ? &by_size.at(128).dsc : nullptr;
  }
  const lv_image_dsc_t* plant_photo_sized(std::string_view id, int diameter) const override {
    if (!sized || id != "fern") return nullptr;
    auto it = by_size.find(diameter);
    return it == by_size.end() ? nullptr : &it->second.dsc;
  }
};

// Draw the small config with `photos` and return the framebuffer pixel at the fern pin's centre.
// Counts LVGL's "format not enabled" warnings into g_not_enabled while it draws.
std::uint16_t fern_centre_pixel(const ImageProvider& photos) {
  host::HeadlessDisplay display{1280, 800};
  // After the display exists: the first lv_init() resets LVGL's print callback.
  g_not_enabled = 0;
  lv_log_register_print_cb(count_lvgl_warnings);
  Config config = test::small_config();
  RecordingSink sink;
  HeatRasterizer raster{config};
  std::vector<Rgb565> heat(raster.pixel_count(), 0);
  lv_image_dsc_t heat_dsc{};
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
  Screen screen(display.screen(), config, &heat_dsc, deps);
  ViewModel vm = test::temperature_vm(config);
  vm.pins[1].on_active_layer = true;  // drawn at full strength, so the pixel is the photo's own
  screen.update(vm, HeaderText{"09:00", "House"}, 1000);
  display.render();
  const CanvasRect& a = config.canvas.map_area;
  const int x = a.x + static_cast<int>(0.1 * a.width);
  const int y = a.y + static_cast<int>(0.1 * a.height);
  const std::uint16_t px =
      display.framebuffer()[static_cast<std::size_t>(y) * 1280U + static_cast<std::size_t>(x)];
  lv_log_register_print_cb(nullptr);
  return px;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

TEST_CASE("pre-sized RGB565 photos are drawn without a transform") {
  DevicePhotos photos(true);
  const std::uint16_t px = fern_centre_pixel(photos);
  CHECK(g_not_enabled == 0);
  CHECK(px == DevicePhotos::kColour);
}

TEST_CASE("an RGB565 photo that must be scaled hits the device's missing format, here too") {
  // The positive control for the test above: without pre-sized photos the host now fails the way
  // the panel did, so a regression to scaling shows up on a laptop.
  DevicePhotos photos(false);
  const std::uint16_t px = fern_centre_pixel(photos);
  CHECK(g_not_enabled > 0);
  CHECK(px != DevicePhotos::kColour);
}

// ---- pulses and drawing cost (components/hearthmap/README.md, "Measuring") ----------------------

namespace {

std::size_t timer_count() {
  std::size_t n = 0;
  for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) ++n;
  return n;
}

std::uint64_t g_invalidated = 0;
void count_invalidation(lv_event_t* e) {
  const auto* a = static_cast<const lv_area_t*>(lv_event_get_param(e));
  if (a != nullptr) g_invalidated += lv_area_get_size(a);
}

}  // namespace

TEST_CASE("pulses are sprites: smooth frames from the tick, no LVGL animation") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  vm.rooms[0].pulse = Severity::Warn;
  rig.show(vm);
  const auto& w = rig.w();
  REQUIRE_FALSE(hidden(w.pin_pulse[0]));
  REQUIRE_FALSE(hidden(w.room_pulse[0]));
  // The ring is an opaque RGB565 image, the size of the ring's reach.
  const auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(w.pin_pulse[0]));
  REQUIRE(dsc != nullptr);
  CHECK(dsc->header.cf == LV_COLOR_FORMAT_RGB565);
  CHECK(lv_obj_get_width(w.pin_pulse[0]) == static_cast<std::int32_t>(dsc->header.w));

  // A pixel 40 px below the pin's centre (clear of its value pill and badge): the ring passes it
  // once a cycle.
  lv_area_t tile;
  lv_obj_get_coords(w.pin_pulse[0], &tile);
  const CanvasRect& a = rig.config.canvas.map_area;
  const auto cx = static_cast<std::int32_t>(a.x + 0.25 * a.width);  // temperature pin 0
  const auto cy = static_cast<std::int32_t>(a.y + 0.25 * a.height);
  const std::size_t probe = static_cast<std::size_t>(cy + 40 - tile.y1) * dsc->header.w +
                            static_cast<std::size_t>(cx - tile.x1);
  std::vector<std::uint16_t> seen;
  std::set<const std::uint16_t*> pointers;
  const PulseCounters before = rig.screen->pulse_counters();
  for (std::uint32_t t = 0; t <= 2 * 1584; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
    CHECK(lv_anim_count_running() == 0);
    // Frames are precomputed: the descriptor points at the frame shown now, a different block of
    // memory for each frame (animating is a pointer swap, not a recomposition).
    const auto* px = reinterpret_cast<const std::uint16_t*>(dsc->data);
    pointers.insert(px);
    if (seen.empty() || seen.back() != px[probe]) seen.push_back(px[probe]);
  }
  const PulseCounters after = rig.screen->pulse_counters();
  // Two cycles of 48 ring frames and of 24 room steps, at the frame rate memory allows
  // (frame_divisor), all of them precomputed.
  const std::uint32_t frames = after.frames - before.frames;
  const std::uint32_t div = after.frame_divisor;
  MESSAGE("frames " << frames << " divisor " << div);
  CHECK(frames >= (2 * 48 + 2 * 24) / div - 4);
  CHECK(frames <= (2 * 48 + 2 * 24) / div + 4);
  if (div > 1) CHECK(after.dropped == before.dropped);  // a 16 ms tick never skips a 64 ms frame
  CHECK(seen.size() >= 4);  // the ring sweeps over the probe, in several shades
  CHECK(pointers.size() >= 48 / after.frame_divisor);  // every frame of the cycle was shown
  CHECK(after.tile_bytes > 0);
}

TEST_CASE("bisect modes: still pulses keep one frame; off pulses capture and compose nothing") {
  ViewModel vm;
  auto pulsing = [&vm](const Config& c) {
    vm = test::temperature_vm(c);
    vm.pins[0].active_value = 33.0;
    vm.pins[0].active_severity = Severity::Danger;
    vm.rooms[0].pulse = Severity::Warn;
  };
  SUBCASE("still") {
    Rig rig;
    rig.screen->set_pulse_mode(PulseMode::Still);
    pulsing(rig.config);
    rig.show(vm);
    const auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(rig.w().pin_pulse[0]));
    REQUIRE(dsc != nullptr);
    std::set<const std::uint8_t*> pointers;
    for (std::uint32_t t = 0; t <= 2 * 1584; t += 16) {
      rig.display.set_time_ms(t);
      rig.display.run_timers();
      pointers.insert(dsc->data);
    }
    const PulseCounters c = rig.screen->pulse_counters();
    CHECK(c.captures > 0);        // the sprites were captured and composed as usual
    CHECK(c.tile_bytes > 0);      // and hold their frames
    CHECK(pointers.size() == 1);  // but the ring never moved on from one frame
    CHECK(rig.screen->verify_pulse_frames());
  }
  SUBCASE("off") {
    Rig rig;
    rig.screen->set_pulse_mode(PulseMode::Off);
    pulsing(rig.config);
    rig.show(vm);
    for (std::uint32_t t = 0; t <= 2 * 1584; t += 16) {
      rig.display.set_time_ms(t);
      rig.display.run_timers();
    }
    const PulseCounters c = rig.screen->pulse_counters();
    CHECK(c.captures == 0);
    CHECK(c.frames == 0);
    CHECK(rig.screen->bake_ready());  // the map bake is not a pulse: it still runs
    CHECK(rig.screen->verify_pulse_frames());
  }
  SUBCASE("animated, the control") {
    Rig rig;
    pulsing(rig.config);
    rig.show(vm);
    for (std::uint32_t t = 0; t <= 1584; t += 16) {
      rig.display.set_time_ms(t);
      rig.display.run_timers();
    }
    CHECK(rig.screen->pulse_counters().captures > 0);
    CHECK(rig.screen->pulse_counters().frames > 10);
  }
}

namespace {

// The outline tiles showing now (screen px), in the outline container's order.
std::vector<lv_area_t> visible_outline_tiles(const ScreenWidgets& w) {
  std::vector<lv_area_t> out;
  const std::uint32_t n = lv_obj_get_child_count(w.outline_pulse);
  for (std::uint32_t i = 0; i < n; ++i) {
    lv_obj_t* t = lv_obj_get_child(w.outline_pulse, static_cast<std::int32_t>(i));
    if (lv_obj_is_hidden(t)) continue;
    lv_area_t a;
    lv_obj_get_coords(t, &a);
    out.push_back(a);
  }
  return out;
}

constexpr std::int32_t kOutlineCell = 64;  // PulseLayer::kCellPx

bool any_overlap(const std::vector<lv_area_t>& tiles) {
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    for (std::size_t j = i + 1; j < tiles.size(); ++j) {
      const lv_area_t& a = tiles[i];
      const lv_area_t& b = tiles[j];
      if (a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("a room's pulse outline invalidates strips along its walls, never its interior") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.rooms[0].pulse = Severity::Danger;
  rig.show(vm);
  lv_obj_update_layout(rig.display.screen());
  const auto& w = rig.w();
  // West room: map (0,0)-(0.5,0.5) of 864 x 720 at (40, 76): 432 x 360 px. The outline is drawn
  // by the cells of a 64 px grid its walls pass through: about (432 + 360) * 2 / 64 of them.
  const std::vector<lv_area_t> tiles = visible_outline_tiles(w);
  MESSAGE("outline tiles " << tiles.size());
  CHECK(tiles.size() >= 2U * (432U + 360U) / 64U - 4U);
  CHECK(tiles.size() <= 2U * (432U + 360U) / 64U + 12U);
  CHECK_FALSE(any_overlap(tiles));  // so nothing drawn later reaches into one (presentable)
  std::uint64_t area = 0;
  for (const lv_area_t& a : tiles) {
    // Every tile lies inside one cell (a corner cell holds two walls; others a strip).
    CHECK(std::max(lv_area_get_width(&a), lv_area_get_height(&a)) <= kOutlineCell);
    area += lv_area_get_size(&a);
  }
  CHECK(area <= 2U * (432U + 360U + 64U) * 32U);  // the perimeter, at most 32 px wide
  CHECK(area < 432U * 360U / 3U);                 // a third of the room

  // One step of the pulse invalidates no more than those strips (plus their draw margin).
  lv_display_add_event_cb(rig.display.display(), count_invalidation, LV_EVENT_INVALIDATE_AREA,
                          nullptr);
  rig.display.render();
  g_invalidated = 0;
  for (std::uint32_t t = 0; t <= 2800; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
  }
  CHECK(g_invalidated > 0);  // it does pulse
  // At most 15 steps of the outline in that window, each exactly its strips.
  CHECK(g_invalidated <= 45U * area);  // at most 44 steps of 64 ms in 2.8 s, each its strips
}

TEST_CASE("a pulse that clears leaves nothing drawn and stops the pulse clock") {
  Rig rig;
  const std::size_t timers_before = timer_count();
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  vm.rooms[1].pulse = Severity::Danger;
  rig.show(vm);
  CHECK(timer_count() == timers_before + 1);
  const auto& w = rig.w();

  vm.pins[0].active_severity = Severity::Ok;
  vm.pins[0].active_value = 21.0;
  vm.rooms[1].pulse = Severity::Ok;
  rig.show(vm);
  CHECK(hidden(w.pin_pulse[0]));
  CHECK(hidden(w.room_pulse[1]));
  lv_display_add_event_cb(rig.display.display(), count_invalidation, LV_EVENT_INVALIDATE_AREA,
                          nullptr);
  rig.display.render();
  g_invalidated = 0;
  for (std::uint32_t t = 0; t <= 2 * 1584; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
  }
  CHECK(g_invalidated == 0);  // nothing left moving
  // The cleared pulses gave their memory back (the budget no longer counts them).
  CHECK(rig.screen->pulse_counters().tile_bytes == 0);
  CHECK(timer_count() == timers_before);  // the clock stopped itself
  CHECK(hidden(w.pin_pulse[0]));          // and nothing came back
  CHECK(hidden(w.room_pulse[1]));

  // A pulse can start again after stopping.
  vm.rooms[1].pulse = Severity::Warn;
  rig.show(vm);
  CHECK_FALSE(hidden(w.room_pulse[1]));
  CHECK(timer_count() == timers_before + 1);
}

TEST_CASE("the heat image tells LVGL it covers the map, unless it is veiled") {
  Rig rig;
  rig.show(test::temperature_vm(rig.config));
  lv_obj_update_layout(rig.display.screen());
  const auto& w = rig.w();
  // Map area 864 x 720 at (40, 76); away from pins, and inside one veil strip (rows 495..539).
  lv_area_t inside{400, 580, 420, 600};
  // The image is drawn as strips (children of heat_image): the one holding the area covers it.
  lv_obj_t* top = lv_refr_get_top_obj(&inside, rig.display.screen());
  REQUIRE(top != nullptr);
  CHECK(lv_obj_get_parent(top) == w.heat_image);
  lv_area_t across{20, 580, 60, 600};  // straddles the map's left edge
  top = lv_refr_get_top_obj(&across, rig.display.screen());
  CHECK((top == nullptr || lv_obj_get_parent(top) != w.heat_image));
  SourceStatus dim;
  dim.heat_behind = true;
  rig.screen->update(test::temperature_vm(rig.config), rig.header, 1000, dim);
  lv_obj_update_layout(rig.display.screen());
  // Hidden while behind: LVGL starts at the background (a fill, not the image).
  top = lv_refr_get_top_obj(&inside, rig.display.screen());
  CHECK((top == nullptr || lv_obj_get_parent(top) != w.heat_image));
}

TEST_CASE("a swapped heat buffer is drawn from the new pixels") {
  Rig rig;
  for (auto& p : rig.heat) p = 0xF800;                // red
  std::vector<Rgb565> back(rig.heat.size(), 0x07E0);  // green
  rig.show(test::temperature_vm(rig.config));
  auto px_at = [&](int map_y) {
    rig.display.render();
    return rig.display.framebuffer()[static_cast<std::size_t>(76 + map_y) * 1280U + 41U + 300U];
  };
  CHECK(px_at(5) == 0xF800);
  CHECK(px_at(700) == 0xF800);
  // The embedder points its descriptor at the other buffer, as the device does after a rebuild.
  rig.heat_dsc.data = reinterpret_cast<const std::uint8_t*>(back.data());
  rig.screen->invalidate_heat();
  CHECK(px_at(5) == 0x07E0);
  CHECK(px_at(700) == 0x07E0);  // every strip, not just the first
}

TEST_CASE("a diagonal wall's pulse is cut into small pieces, so it never invalidates the room") {
  Config c = test::small_config();
  // A valid triangle whose hypotenuse runs 432 x 360 px across the map.
  c.rooms[0].polygon = {{0.0, 0.0}, {0.5, 0.0}, {0.5, 0.5}};
  Rig rig(c);
  ViewModel vm = test::temperature_vm(rig.config);
  vm.rooms[0].pulse = Severity::Danger;
  rig.show(vm);
  lv_obj_update_layout(rig.display.screen());
  const auto& w = rig.w();
  // Cells of the 64 px grid: two axis-aligned edges and the diagonal, each tile inside its cell.
  const std::vector<lv_area_t> tiles = visible_outline_tiles(w);
  CHECK(tiles.size() >= 3U);
  CHECK_FALSE(any_overlap(tiles));
  std::uint64_t area = 0;
  for (const lv_area_t& a : tiles) {
    CHECK(std::max(lv_area_get_width(&a), lv_area_get_height(&a)) <= kOutlineCell);
    area += lv_area_get_size(&a);
  }
  // Under the triangle's own area; one object for the hypotenuse alone covered 443 x 371 px,
  // more than twice that.
  MESSAGE("diagonal room sprite area " << area);
  CHECK(area < 432U * 360U / 2U);
}

namespace {

// Pixels of the framebuffer inside `a` whose distance from (cx, cy) is below `inner` or above
// `outer`, with the ring tile shown and with it hidden: they must be equal.
struct TileDiff {
  std::size_t compared = 0, differ = 0, ring_differ = 0;
};
TileDiff diff_without_tile(Rig& rig, lv_obj_t* tile, float cx, float cy, float inner, float outer) {
  lv_area_t a;
  lv_obj_get_coords(tile, &a);
  rig.display.render();
  const std::vector<std::uint16_t> with = rig.display.framebuffer();
  lv_obj_set_hidden(tile, true);
  rig.display.render();
  const std::vector<std::uint16_t>& without = rig.display.framebuffer();
  TileDiff d;
  for (std::int32_t y = a.y1; y <= a.y2; ++y) {
    for (std::int32_t x = a.x1; x <= a.x2; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * 1280U + static_cast<std::size_t>(x);
      const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
      const float r = std::sqrt(dx * dx + dy * dy);
      if (r >= inner && r <= outer) {
        d.ring_differ += with[i] != without[i] ? 1U : 0U;
        continue;
      }
      ++d.compared;
      d.differ += with[i] != without[i] ? 1U : 0U;
    }
  }
  lv_obj_set_hidden(tile, false);
  return d;
}

}  // namespace

TEST_CASE("a ring sprite shows exactly what lies under it, plus the ring, and follows the map") {
  Rig rig;
  for (auto& p : rig.heat) p = 0xF800;  // red
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  rig.show(vm);
  rig.display.set_time_ms(500);
  rig.display.run_timers();
  const auto& w = rig.w();
  const CanvasRect& a = rig.config.canvas.map_area;
  const float cx = static_cast<float>(a.x) + 0.25f * static_cast<float>(a.width);
  const float cy = static_cast<float>(a.y) + 0.25f * static_cast<float>(a.height);
  // Inside 22 px is the pin; beyond 57 px nothing of the ring reaches. Both must match LVGL's own
  // drawing exactly (the pin, its value pill and badge, the map); between them the ring shows.
  TileDiff d = diff_without_tile(rig, w.pin_pulse[0], cx, cy, 22.0f, 57.0f);
  CHECK(d.compared > 1000);
  CHECK(d.differ == 0);
  CHECK(d.ring_differ > 50);

  // A new heat image under the sprite: once prepared again, the sprite shows it.
  std::vector<Rgb565> green(rig.heat.size(), 0x07E0);
  rig.heat_dsc.data = reinterpret_cast<const std::uint8_t*>(green.data());
  rig.screen->invalidate_heat();
  while (rig.screen->prepare_step()) {
  }
  rig.display.set_time_ms(600);
  rig.display.run_timers();
  d = diff_without_tile(rig, w.pin_pulse[0], cx, cy, 22.0f, 57.0f);
  CHECK(d.differ == 0);
}

TEST_CASE("walls and labels baked into the heat image look as LVGL drew them") {
  Rig rig;
  for (auto& p : rig.heat) p = 0x3186;  // a dark grey-blue, like neutral heat
  const ViewModel vm = test::temperature_vm(rig.config);
  rig.show(vm);
  REQUIRE(rig.screen->bake_ready());  // Rig::show() prepares everything
  rig.display.render();
  const std::vector<std::uint16_t> drawn = rig.display.framebuffer();

  // The embedder lays the bake over its image and says so: the objects step aside.
  CHECK(rig.screen->bake_rows(rig.heat.data(), rig.raster.width(), 0, rig.raster.height()));
  SourceStatus baked;
  baked.heat_baked = true;
  rig.screen->update(vm, rig.header, 1000, baked);
  rig.screen->invalidate_heat();
  CHECK(hidden(rig.w().room_label[0]));
  rig.display.render();
  const std::vector<std::uint16_t>& now = rig.display.framebuffer();
  // Inside the map (864 x 720 at 40, 76) every pixel within a step or two of LVGL's own drawing
  // (the coverage is recovered from 6 bits of green). Unbaked walls or labels differ by far more.
  std::size_t off = 0, compared = 0;
  // Pins are left out: the fixture's 8 px photo is scaled, which this LVGL build cannot do for
  // RGB565 (D-039), and what it leaves behind was seen to differ between two renders on Linux.
  auto near_pin = [&](std::int32_t x, std::int32_t y) {
    for (const Pin& pin : rig.config.pins) {
      const double px = 40 + pin.position.x * 864, py = 76 + pin.position.y * 720;
      if (std::abs(x - px) < 60 && std::abs(y - py) < 40) return true;
    }
    return false;
  };
  for (std::int32_t y = 76; y < 76 + 720; ++y) {
    for (std::int32_t x = 40; x < 40 + 864; ++x) {
      if (near_pin(x, y)) continue;
      const std::size_t i = static_cast<std::size_t>(y) * 1280U + static_cast<std::size_t>(x);
      const int dr = std::abs(((drawn[i] >> 11) & 31) - ((now[i] >> 11) & 31));
      const int dg = std::abs(((drawn[i] >> 5) & 63) - ((now[i] >> 5) & 63));
      const int db = std::abs((drawn[i] & 31) - (now[i] & 31));
      ++compared;
      if (dr > 1 || dg > 2 || db > 1) {
        if (off < 8) {
          MESSAGE("bake off at " << x << "," << y << ": LVGL " << drawn[i] << " baked " << now[i]
                                 << " heat " << rig.heat[0]);
        }
        ++off;
      }
    }
  }
  CHECK(compared > 500000);
  CHECK(off == 0);

  // Told the image is not baked again (a fresh image without walls): the objects come back.
  rig.screen->update(vm, rig.header, 1000, SourceStatus{});
  CHECK_FALSE(hidden(rig.w().room_label[0]));
}

// One of small_config()'s top rooms' outline sprites fits, two do not, at the coarsest rate with
// room for one replacement set (PulseLayer::need_at).
constexpr std::size_t kOneRoomBudget = 500U * 1024U;

TEST_CASE("a pulse waiting for memory gets it when another pulse stops") {
  // Room for one room outline's sprites, not two (west and east rooms are 432 x 360 px each).
  Config c = test::small_config();
  host::HeadlessDisplay display{1280, 800};
  RecordingSink sink;
  HeatRasterizer raster{c};
  std::vector<Rgb565> heat(raster.pixel_count(), 0);
  lv_image_dsc_t dsc{};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"  // lv_image_header_t's bit-fields
#endif
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = static_cast<std::uint32_t>(raster.width());
  dsc.header.h = static_cast<std::uint32_t>(raster.height());
  dsc.header.stride = static_cast<std::uint32_t>(raster.width() * 2);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  dsc.data_size = static_cast<std::uint32_t>(heat.size() * 2);
  dsc.data = reinterpret_cast<const std::uint8_t*>(heat.data());
  ScreenDeps deps;
  deps.fonts = builtin_fonts();
  deps.sink = &sink;
  deps.pulse_memory_bytes = kOneRoomBudget;
  Screen screen(display.screen(), c, &dsc, deps);
  ViewModel vm = test::temperature_vm(c);
  const HeaderText header{"09:00", ""};
  auto show = [&]() {
    screen.update(vm, header, 1000);
    while (screen.prepare_step()) {
    }
  };
  vm.rooms[0].pulse = Severity::Warn;
  vm.rooms[1].pulse = Severity::Warn;
  show();
  const auto& w = screen.widgets();
  // Shown: pulsing, and a tile on the middle of its own top wall showing (the shared wall's
  // tiles show for either room).
  auto shown = [&](std::size_t r) {
    const lv_point_t probe{r == 0 ? 40 + 216 : 40 + 432 + 216, 82};  // tiles start at y 80
    bool tile = false;
    for (const lv_area_t& a : visible_outline_tiles(w)) {
      tile = tile || (probe.x >= a.x1 && probe.x <= a.x2 && probe.y >= a.y1 && probe.y <= a.y2);
    }
    return !lv_obj_is_hidden(w.room_pulse[r]) && tile;
  };
  MESSAGE("sprite bytes with one room " << screen.pulse_counters().tile_bytes);
  CHECK(shown(0));
  CHECK_FALSE(shown(1));  // over budget: waits
  vm.rooms[0].pulse = Severity::Ok;
  show();
  CHECK_FALSE(shown(0));
  CHECK(shown(1));  // the freed memory went to the waiting room, with no change of its own
}

TEST_CASE("a press on a pin is acknowledged before release; elsewhere nothing shows") {
  Rig rig;
  rig.show(test::temperature_vm(rig.config));
  // Pin 0 (temperature) at map (0.25, 0.25) of 864 x 720 at (40, 76).
  const std::int32_t x = 40 + 216, y = 76 + 180;
  auto marker = [&]() -> lv_obj_t* {
    // The last child of the map is the press ring.
    lv_obj_t* map = rig.w().map;
    return lv_obj_get_child(map, static_cast<std::int32_t>(lv_obj_get_child_count(map)) - 1);
  };
  REQUIRE(marker() != nullptr);
  CHECK(hidden(marker()));
  rig.display.press(x, y);
  CHECK_FALSE(hidden(marker()));
  lv_obj_update_layout(rig.display.screen());
  lv_area_t a;
  lv_obj_get_coords(marker(), &a);
  CHECK(a.x1 < x);
  CHECK(a.x2 > x);
  CHECK(rig.sink.events.empty());  // nothing sent yet: the tap is not complete
  rig.display.release();
  CHECK(hidden(marker()));
  CHECK(rig.sink.events.size() == 1);  // and the tap itself still goes through core (R12)
  // A press on empty map shows nothing.
  rig.display.press(40 + 700, 76 + 600);
  CHECK(hidden(marker()));
  rig.display.release();
}

namespace {

// Reads every pixel of every tile it is handed (so a sanitiser sees a freed frame), refuses every
// third one (so the fallback path runs too), and counts pixels of frames not yet composed.
struct ReadingPresenter final : Presenter {
  std::uint64_t calls = 0, uncomposed = 0, sum = 0;
  bool present(int, int, const View565& tile) override {
    ++calls;
    for (int y = 0; y < tile.h; ++y) {
      for (int x = 0; x < tile.w; ++x) {
        const std::uint16_t v = tile.px[static_cast<std::size_t>(y * tile.stride + x)];
        sum += v;
        uncomposed += v == static_cast<std::uint16_t>(0xF81F) ? 1U : 0U;
      }
    }
    return calls % 3U != 0U;
  }
};

// Every tile, every shown frame, through cycles and the moments that rebuild or free frame sets.
void walk_pulses(std::size_t memory, std::uint32_t divisor) {
  CAPTURE(memory);
  CAPTURE(divisor);
  Rig rig(test::small_config(), memory, divisor);
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  vm.rooms[0].pulse = Severity::Danger;
  vm.rooms[1].pulse = Severity::Danger;  // shares a wall with room 0
  vm.rooms[2].pulse = Severity::Warn;
  rig.show(vm);
  rig.screen->set_frames_deferred(true);
  ReadingPresenter presenter;
  const char* why = "";
  bool ok = true;
  std::uint32_t t = 0;
  const std::uint32_t steps[] = {16, 16, 7, 25, 16, 1, 31};
  for (std::size_t k = 0; t < 20000; ++k) {
    t += steps[k % 7];
    rig.display.set_time_ms(t);
    // Something changes mid-cycle every 800 ms: a new heat image under every tile, another
    // layer's pulses, a ring starting and one stopping; preparation is sliced meanwhile.
    if (k % 50 == 10) rig.screen->invalidate_heat();
    if (k % 50 == 20) {
      vm.rooms[1].pulse = vm.rooms[1].pulse == Severity::Ok ? Severity::Danger : Severity::Ok;
      vm.rooms[2].pulse = vm.rooms[2].pulse == Severity::Ok ? Severity::Warn : Severity::Ok;
      rig.screen->update(vm, rig.header, 1000);
    }
    if (k % 50 == 30) {
      vm.pins[1].active_severity =
          vm.pins[1].active_severity == Severity::Danger ? Severity::Ok : Severity::Danger;
      rig.screen->update(vm, rig.header, 1000);
    }
    if (k % 50 == 40) {
      vm.pins[0].active_severity =
          vm.pins[0].active_severity == Severity::Danger ? Severity::Ok : Severity::Danger;
      rig.screen->update(vm, rig.header, 1000);
    }
    rig.display.run_timers();
    for (int i = 0; i < 40; ++i) (void)rig.screen->prepare_step();  // a device slice's worth
    ok = ok && rig.screen->verify_pulse_frames(&why);
    rig.screen->show_due_frames(presenter);
    ok = ok && rig.screen->verify_pulse_frames(&why);
    ok = ok && rig.screen->pulse_canary_failures() == 0;
    if (k % 3 == 0) rig.display.render();  // LVGL reads the descriptors of fallen-back tiles
    if (!ok) break;
  }
  INFO("first problem: " << std::string(why) << " at " << t << " ms");
  CHECK(ok);
  CHECK(presenter.calls > 100);
  CHECK(presenter.uncomposed == 0);
  const PulseCounters c = rig.screen->pulse_counters();
  MESSAGE("divisor " << c.frame_divisor << ", " << presenter.calls << " frames handed over, "
                     << c.not_ready << " not ready");
  if (divisor > 1 || memory > 8U * 1024U * 1024U) CHECK(c.frame_divisor == divisor);
  CHECK(c.not_ready == 0);  // every index the clock asks for exists in its set
}

}  // namespace

TEST_CASE("every shown pulse frame is a whole composed frame, at every rate and through changes") {
  for (std::uint32_t divisor : {1U, 2U, 4U, 8U}) walk_pulses(64U * 1024U * 1024U, divisor);
  // Memory pressure: rings take memory from outlines, which wait and come back.
  walk_pulses(1536U * 1024U, 1U);  // the divisor is memory's choice here
}

TEST_CASE("pulse sprites stay inside their memory budget while frames are recomposed") {
  // The old set keeps playing while its replacement is composed: both must fit, at every budget.
  for (std::size_t budget : {256U * 1024U, 400U * 1024U, 700U * 1024U, 1200U * 1024U}) {
    CAPTURE(budget);
    Rig rig(test::small_config(), budget, 1);
    ViewModel vm = test::temperature_vm(rig.config);
    vm.pins[0].active_value = 33.0;
    vm.pins[0].active_severity = Severity::Danger;
    vm.rooms[0].pulse = Severity::Danger;
    vm.rooms[1].pulse = Severity::Warn;
    std::size_t peak = 0;
    rig.screen->update(vm, rig.header, 1000);
    for (int round = 0; round < 3; ++round) {
      while (rig.screen->prepare_step()) {
        peak = std::max(peak, rig.screen->pulse_counters().tile_bytes);
      }
      peak = std::max(peak, rig.screen->pulse_counters().tile_bytes);
      rig.screen->invalidate_heat();  // every tile recomposed, the old sets kept meanwhile
      rig.display.set_time_ms(static_cast<std::uint32_t>(100 * (round + 1)));
      rig.display.run_timers();
    }
    MESSAGE("budget " << budget << ", peak " << peak);
    if (budget >= 700U * 1024U) CHECK(peak > 0);  // something was admitted to measure
    CHECK(peak <= budget);
  }
}

TEST_CASE("fallback invalidations stay under LVGL's area limit until a refresh") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  for (auto& r : vm.rooms) r.pulse = Severity::Danger;  // dozens of outline cells
  rig.show(vm);
  rig.screen->set_frames_deferred(true);
  rig.display.render();
  NoPresenter none;  // every frame falls back
  lv_display_t* disp = rig.display.display();
  std::uint32_t most = 0;
  bool whole = false;
  for (std::uint32_t t = 16; t <= 1600; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
    // The main loop drains the queue on every pass, several times between two refreshes.
    for (int pass = 0; pass < 3; ++pass) {
      Screen::ShownFrames tally;
      rig.screen->show_due_frames(none, &tally);
      most = std::max(most, disp->inv_p);
      const lv_area_t& a = disp->inv_areas[0];
      whole = whole || (disp->inv_p == 1 && a.x1 == 0 && a.y1 == 0 && a.x2 == 1279 && a.y2 == 799);
    }
    rig.display.render();
  }
  MESSAGE("most invalidated areas before a refresh: " << most);
  CHECK(most > 0);
  CHECK(most <= 20);
  CHECK_FALSE(whole);  // LVGL never gave up and repainted the whole screen
}

TEST_CASE("a held press is never captured into a pulse sprite") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;  // pin 0's ring tile holds the press ring's area
  rig.show(vm);
  const std::int32_t x = 40 + 216, y = 76 + 180;  // pin 0
  rig.display.render();
  const std::vector<std::uint16_t> before = rig.display.framebuffer();
  // Pressed and held while the sprites are recaptured (a new heat image under them).
  rig.display.press(x, y);
  rig.screen->invalidate_heat();
  while (rig.screen->prepare_step()) {
  }
  rig.display.release();
  rig.display.render();
  // Released: the ring tile shows no trace of the press ring (same frame time as before).
  const std::vector<std::uint16_t>& after = rig.display.framebuffer();
  int differ = 0;
  for (std::int32_t yy = y - 40; yy <= y + 40; ++yy) {
    for (std::int32_t xx = x - 40; xx <= x + 40; ++xx) {
      const auto i = static_cast<std::size_t>(yy) * 1280U + static_cast<std::size_t>(xx);
      differ += before[i] != after[i] ? 1 : 0;
    }
  }
  CHECK(differ == 0);
}

namespace {

struct CountingPresenter final : Presenter {
  int calls = 0;
  bool present(int, int, const View565& tile) override {
    ++calls;
    return tile.px != nullptr;
  }
};

}  // namespace

TEST_CASE("deferred pulse frames are presented when nothing covers them, without invalidating") {
  Rig rig;
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  rig.show(vm);
  rig.screen->set_frames_deferred(true);
  lv_display_add_event_cb(rig.display.display(), count_invalidation, LV_EVENT_INVALIDATE_AREA,
                          nullptr);
  rig.display.render();
  g_invalidated = 0;
  CountingPresenter presenter;
  Screen::ShownFrames tally;
  for (std::uint32_t t = 0; t <= 1600; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
    rig.screen->show_due_frames(presenter, &tally);
  }
  MESSAGE("presented " << tally.presented << ", fell back " << tally.invalidated);
  CHECK(tally.presented >= 10);  // a ring's frames, straight to the panel
  CHECK(presenter.calls == static_cast<int>(tally.presented));
  CHECK(tally.invalidated == 0);
  CHECK(g_invalidated == 0);  // and LVGL was asked to repaint nothing

  // A presenter that refuses: every frame falls back to an invalidation, so none is lost.
  NoPresenter none;
  Screen::ShownFrames refused;
  for (std::uint32_t t = 1616; t <= 2400; t += 16) {
    rig.display.set_time_ms(t);
    rig.display.run_timers();
    rig.screen->show_due_frames(none, &refused);
  }
  CHECK(refused.presented == 0);
  CHECK(refused.invalidated >= 10);
  CHECK(g_invalidated > 0);
}

namespace {

/// The system heap behind a switch: while `refuse` is set every allocation is refused, as a full
/// PSRAM would refuse it on the panel.
struct SwitchedMemory final : BulkMemory {
  bool refuse = false;
  std::size_t refused = 0;
  void* allocate(std::size_t bytes, std::size_t align) override {
    if (refuse) {
      ++refused;
      return nullptr;
    }
    return system_bulk_memory().allocate(bytes, align);
  }
  void deallocate(void* p, std::size_t bytes) override {
    system_bulk_memory().deallocate(p, bytes);
  }
};

}  // namespace

TEST_CASE("sprite buffers the memory port refuses stop the animation, never break a frame") {
  SwitchedMemory mem;
  REQUIRE(set_bulk_memory(&mem));
  {
    Rig rig(test::small_config(), 64U * 1024U * 1024U, 1);
    ViewModel vm = test::temperature_vm(rig.config);
    vm.pins[0].active_value = 33.0;
    vm.pins[0].active_severity = Severity::Danger;
    vm.rooms[0].pulse = Severity::Danger;
    rig.show(vm);
    REQUIRE(rig.screen->pulse_counters().rings_shown == 1);
    REQUIRE(rig.screen->pulse_counters().alloc_failures == 0);
    const char* why = "";

    // Everything under the sprites changes while the port refuses: preparation ends (no tile
    // asks again at once), every tile steps aside, and no frame shown is anything but whole.
    mem.refuse = true;
    rig.screen->invalidate_heat();
    int steps = 0;
    while (rig.screen->prepare_step() && steps < 10000) ++steps;
    CHECK(steps < 10000);
    CHECK(mem.refused > 0);
    PulseCounters c = rig.screen->pulse_counters();
    CHECK(c.alloc_failures > 0);
    CHECK(c.rings_shown == 0);
    CHECK(c.rooms_animated == 0);
    rig.display.set_time_ms(500);
    rig.display.run_timers();
    rig.display.render();
    CHECK(rig.screen->verify_pulse_frames(&why));
    INFO(why);
    CHECK(rig.screen->pulse_canary_failures() == 0);

    // Memory again, and the next change under them brings the pulses back.
    mem.refuse = false;
    rig.screen->invalidate_heat();
    while (rig.screen->prepare_step()) {
    }
    c = rig.screen->pulse_counters();
    CHECK(c.rings_shown == 1);
    CHECK(c.rooms_animated == 1);
    CHECK(rig.screen->verify_pulse_frames(&why));
  }
  CHECK(bulk_stats().in_use == 0);
  REQUIRE(set_bulk_memory(nullptr));
}

namespace {

/// The system heap; while `refuse_small` is set it refuses every block under 512 bytes, the
/// size of the small containers a vector grows into, and passes pixel-sized ones.
struct SmallRefusingMemory final : BulkMemory {
  bool refuse_small = false;
  std::size_t refused = 0;
  void* allocate(std::size_t bytes, std::size_t align) override {
    if (refuse_small && bytes < 512) {
      ++refused;
      return nullptr;
    }
    return system_bulk_memory().allocate(bytes, align);
  }
  void deallocate(void* p, std::size_t bytes) override {
    system_bulk_memory().deallocate(p, bytes);
  }
};

}  // namespace

TEST_CASE("building pulse sprites never grows a container the port could refuse") {
  // A refused BulkVector growth aborts (memory.h), so the pulse layer's per-tile lists are
  // sized when it is built; building sprites afterwards allocates only pixel-sized buffers,
  // which fail softly.
  SmallRefusingMemory mem;
  REQUIRE(set_bulk_memory(&mem));
  {
    Config c = test::small_config();
    c.pins[1].position = MapPoint{0.25, 0.33};  // the fern's ring overlaps the west pin's
    Rig rig(c, 64U * 1024U * 1024U, 1);
    mem.refuse_small = true;
    ViewModel vm = test::temperature_vm(rig.config);
    for (std::size_t i : {0U, 1U, 3U}) {
      vm.pins[i].on_active_layer = true;
      vm.pins[i].active_value = 33.0;
      vm.pins[i].active_severity = Severity::Danger;
    }
    vm.rooms[0].pulse = Severity::Danger;
    vm.rooms[1].pulse = Severity::Warn;
    rig.show(vm);  // aborted in a ring's or a cell's list before they were sized up front
    const PulseCounters pc = rig.screen->pulse_counters();
    CHECK(pc.rings_shown == 3);
    CHECK(pc.rooms_animated == 2);
    CHECK(pc.alloc_failures == 0);
    CHECK(rig.screen->bake_ready());  // the bake's lists were taken before the port refused
    const char* why = "";
    CHECK(rig.screen->verify_pulse_frames(&why));
    INFO(why);
    // A ring and a room stop and start again: their lists keep what they were given.
    vm.pins[1].active_severity = Severity::Ok;
    vm.rooms[0].pulse = Severity::Ok;
    rig.show(vm);
    vm.pins[1].active_severity = Severity::Danger;
    vm.rooms[0].pulse = Severity::Danger;
    rig.show(vm);
    CHECK(rig.screen->pulse_counters().rings_shown == 3);
    CHECK(rig.screen->pulse_counters().rooms_animated == 2);
    CHECK(mem.refused == 0);
    mem.refuse_small = false;
  }
  CHECK(bulk_stats().in_use == 0);
  REQUIRE(set_bulk_memory(nullptr));
}

TEST_CASE("paused sprite builds start nothing; the sets already built keep playing") {
  Rig rig(test::small_config(), 64U * 1024U * 1024U, 1);
  ViewModel vm = test::temperature_vm(rig.config);
  vm.pins[0].active_value = 33.0;
  vm.pins[0].active_severity = Severity::Danger;
  rig.show(vm);
  REQUIRE(rig.screen->pulse_counters().rings_shown == 1);
  const std::uint32_t captures = rig.screen->pulse_counters().captures;

  rig.screen->set_sprite_builds_paused(true);
  CHECK(rig.screen->pulse_counters().builds_paused);
  // A room starts pulsing while paused: nothing is captured for it, and the ring plays on.
  vm.rooms[0].pulse = Severity::Danger;
  rig.screen->update(vm, rig.header, 1000);
  CHECK_FALSE(rig.screen->prepare_step());
  CHECK(rig.screen->pulse_counters().captures == captures);
  CHECK(rig.screen->pulse_counters().rings_shown == 1);
  CHECK(rig.screen->pulse_counters().rooms_animated == 0);

  // A set half built when the pause comes is dropped, and built whole after it.
  rig.screen->set_sprite_builds_paused(false);
  CHECK(rig.screen->prepare_step());
  CHECK(rig.screen->prepare_step());
  rig.screen->set_sprite_builds_paused(true);
  CHECK_FALSE(rig.screen->prepare_step());
  rig.screen->set_sprite_builds_paused(false);
  while (rig.screen->prepare_step()) {
  }
  const PulseCounters c = rig.screen->pulse_counters();
  CHECK_FALSE(c.builds_paused);
  CHECK(c.rooms_animated == 1);
  const char* why = "";
  CHECK(rig.screen->verify_pulse_frames(&why));
  INFO(why);
}
