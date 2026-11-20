// view_model.h: hit_test(), apply(), build_view_model(), heat_samples().
#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/config.h"
#include "hearthmap/readings.h"
#include "hearthmap/view_model.h"

using namespace hearthmap;
using namespace hearthmap::test;

namespace {

// A plain config for geometric tests: map area 100..900 x 0..800 on a 1280x800 canvas, two
// rooms side by side (left x 0..0.5, right 0.5..1), a third room overlapping the right one.
Config geo_config() {
  Config c;
  c.canvas.map_area = CanvasRect{100, 0, 800, 800};
  c.rooms = {
      Room{"left", "Left", {{0, 0}, {0.5, 0}, {0.5, 1}, {0, 1}}, false},
      Room{"right", "Right", {{0.5, 0}, {1, 0}, {1, 1}, {0.5, 1}}, false},
      Room{"overlap", "Overlap", {{0.6, 0.6}, {0.9, 0.6}, {0.9, 0.9}, {0.6, 0.9}}, false},
  };
  auto add = [&](const char* id, MapPoint pos, const char* room) {
    Pin p;
    p.id = id;
    p.kind = PinKind::Temperature;
    p.name = id;
    p.position = pos;
    p.room_id = room;
    p.entities = {{Role::Temperature, std::string("sensor.example_") + id}};
    c.pins.push_back(p);
  };
  add("a", {0.25, 0.5}, "left");    // canvas (300, 400)
  add("b", {0.30, 0.5}, "left");    // canvas (340, 400): 40 px right of a
  add("c", {0.75, 0.25}, "right");  // canvas (700, 200)
  return c;
}

CanvasPoint canvas_of(const Config& c, std::size_t pin) {
  return to_canvas(c.canvas.map_area, c.pins[pin].position);
}

const PinView& pin_view(const ViewModel& vm, const Config& c, const char* id) {
  return vm.pins[pin_index(c, id)];
}

std::vector<std::string> hud_ids(const Config& c, const ViewModel& vm) {
  std::vector<std::string> ids;
  for (const auto& item : vm.hud.items) ids.push_back(c.pins[item.pin_index].id);
  return ids;
}

UiState state(Layer layer, std::optional<std::string> overlay = std::nullopt) {
  return UiState{layer, std::move(overlay)};
}

}  // namespace

// --- hit_test --------------------------------------------------------------------------------

TEST_CASE("hit_test: outside the map area is None, even over a pin's radius") {
  const Config c = geo_config();
  CHECK(hit_test(c, {99.9, 400}).kind == HitResult::Kind::None);
  CHECK(hit_test(c, {900, 400}).kind == HitResult::Kind::None);   // right edge exclusive
  CHECK(hit_test(c, {1000, 400}).kind == HitResult::Kind::None);  // the HUD column
  // A pin near the map edge: a tap just outside the area does not reach it.
  Config edge = c;
  edge.pins[0].position = {0.0, 0.5};  // canvas (100, 400)
  CHECK(hit_test(edge, {95, 400}).kind == HitResult::Kind::None);
  CHECK(hit_test(edge, {105, 400}).kind == HitResult::Kind::Pin);
}

TEST_CASE("hit_test: nearest pin within the radius; exactly on the radius counts") {
  const Config c = geo_config();
  const CanvasPoint a = canvas_of(c, 0);
  auto hit = hit_test(c, {a.x - 32.0, a.y});
  CHECK(hit.kind == HitResult::Kind::Pin);
  CHECK(hit.index == 0);
  hit = hit_test(c, {a.x - 32.001, a.y});
  CHECK(hit.kind == HitResult::Kind::Room);  // falls through to the room under it
  CHECK(hit.index == 0);
  // Between a (x 300) and b (x 340): 315 is nearer a, 325 nearer b.
  CHECK(hit_test(c, {315, 400}).index == 0);
  CHECK(hit_test(c, {325, 400}).index == 1);
  // A custom radius.
  CHECK(hit_test(c, {a.x - 10, a.y}, 5.0).kind == HitResult::Kind::Room);
}

TEST_CASE("hit_test: a tap equidistant from two pins goes to the earlier one") {
  const Config c = geo_config();
  const auto hit = hit_test(c, {320, 400});  // 20 px from both a and b
  CHECK(hit.kind == HitResult::Kind::Pin);
  CHECK(hit.index == 0);
  Config swapped = c;
  std::swap(swapped.pins[0], swapped.pins[1]);
  const auto hit2 = hit_test(swapped, {320, 400});
  CHECK(hit2.index == 0);
  CHECK(swapped.pins[hit2.index].id == "b");
}

TEST_CASE("hit_test: coincident pins go to the earlier one") {
  Config c = geo_config();
  c.pins[2].position = c.pins[1].position;
  const auto hit = hit_test(c, canvas_of(c, 1));
  CHECK(hit.index == 1);
}

TEST_CASE("hit_test: pins of every kind are hittable") {
  const Config c = flat_config();
  for (std::size_t i = 0; i < c.pins.size(); ++i) {
    CAPTURE(c.pins[i].id);
    const auto hit = hit_test(c, canvas_of(c, i));
    CHECK(hit.kind == HitResult::Kind::Pin);
    CHECK(hit.index == i);
  }
}

TEST_CASE("hit_test: rooms in config order where they overlap; empty map is None") {
  const Config c = geo_config();
  auto hit = hit_test(c, {100 + 0.75 * 800, 0.75 * 800});  // inside right and overlap
  CHECK(hit.kind == HitResult::Kind::Room);
  CHECK(hit.index == 1);
  Config reordered = c;
  std::swap(reordered.rooms[1], reordered.rooms[2]);
  hit = hit_test(reordered, {100 + 0.75 * 800, 0.75 * 800});
  CHECK(hit.index == 1);
  CHECK(reordered.rooms[1].id == "overlap");

  Config sparse = c;
  sparse.rooms.resize(1);  // only the left half has a room
  CHECK(hit_test(sparse, {800, 700}).kind == HitResult::Kind::None);
}

// --- apply -----------------------------------------------------------------------------------

TEST_CASE("apply: tap a pin opens it; a second tap closes; another pin switches") {
  const Config c = geo_config();
  const UiEvent tap_a = UiEvent::tap_map(canvas_of(c, 0));
  const UiEvent tap_c = UiEvent::tap_map(canvas_of(c, 2));
  UiState s = initial_ui_state(c);
  s = apply(c, s, tap_a);
  CHECK(s.overlay_pin == "a");
  s = apply(c, s, tap_c);
  CHECK(s.overlay_pin == "c");
  s = apply(c, s, tap_c);
  CHECK_FALSE(s.overlay_pin.has_value());
  s = apply(c, s, tap_c);
  CHECK(s.overlay_pin == "c");
}

TEST_CASE("apply: tap a room, empty map or outside the map closes the overlay") {
  const Config c = geo_config();
  for (CanvasPoint p : {CanvasPoint{150, 700}, CanvasPoint{1000, 100}}) {
    CAPTURE(p.x);
    const UiState s = apply(c, state(Layer::Temperature, "a"), UiEvent::tap_map(p));
    CHECK_FALSE(s.overlay_pin.has_value());
    CHECK(s.active_layer == Layer::Temperature);
  }
  Config sparse = c;
  sparse.rooms.resize(1);
  CHECK_FALSE(
      apply(sparse, state(Layer::Temperature, "a"), UiEvent::tap_map({800, 700})).overlay_pin);
}

TEST_CASE("apply: select layer keeps the overlay; a disabled layer is ignored") {
  Config c = geo_config();
  c.layers[index_of(Layer::Air)].enabled = false;
  UiState s = apply(c, state(Layer::Temperature, "a"), UiEvent::select_layer(Layer::Soil));
  CHECK(s.active_layer == Layer::Soil);
  CHECK(s.overlay_pin == "a");
  s = apply(c, s, UiEvent::select_layer(Layer::Air));
  CHECK(s.active_layer == Layer::Soil);
}

TEST_CASE("apply: HUD item opens its pin; an unknown id changes nothing") {
  const Config c = geo_config();
  UiState s = apply(c, state(Layer::Humidity), UiEvent::tap_hud_item("c"));
  CHECK(s.overlay_pin == "c");
  s = apply(c, s, UiEvent::tap_hud_item("c"));  // not a toggle
  CHECK(s.overlay_pin == "c");
  s = apply(c, s, UiEvent::tap_hud_item("nope"));
  CHECK(s.overlay_pin == "c");
  CHECK(s.active_layer == Layer::Humidity);
}

TEST_CASE("apply: close overlay") {
  const Config c = geo_config();
  CHECK_FALSE(apply(c, state(Layer::Soil, "a"), UiEvent::close_overlay()).overlay_pin);
  CHECK_FALSE(apply(c, state(Layer::Soil), UiEvent::close_overlay()).overlay_pin);
}

// --- build_view_model: HUD -------------------------------------------------------------------

TEST_CASE("HUD on thirsty-many: worst first, capped at 4, +2 more, soil pin excluded") {
  const Config c = flat_config();
  const ViewModel vm = build_view_model(c, flat_thirsty_many(), kFlatNow, initial_ui_state(c));
  // Danger: basil 1.0, monstera 0.982, lavender 0.96, pothos 0.906; Warn: snake 0.838, fern.
  CHECK(hud_ids(c, vm) == std::vector<std::string>{"plant_basil", "plant_monstera",
                                                   "plant_lavender", "plant_pothos"});
  CHECK(vm.hud.overflow == 2);
  for (const auto& item : vm.hud.items) CHECK(item.severity == Severity::Danger);
  CHECK(vm.hud.items[0].thirst == 1.0);

  Config wide = c;
  wide.hud.max_items = 20;
  const ViewModel all = build_view_model(wide, flat_thirsty_many(), kFlatNow, initial_ui_state(c));
  CHECK(hud_ids(wide, all) == std::vector<std::string>{"plant_basil", "plant_monstera",
                                                       "plant_lavender", "plant_pothos",
                                                       "plant_snake", "plant_fern"});
  CHECK(all.hud.overflow == 0);
  CHECK(all.hud.items[4].severity == Severity::Warn);
  CHECK(all.hud.items[5].severity == Severity::Warn);

  Config exact = c;
  exact.hud.max_items = 6;  // cap equal to the candidates: no overflow
  CHECK(build_view_model(exact, flat_thirsty_many(), kFlatNow, initial_ui_state(c)).hud.overflow ==
        0);
  exact.hud.max_items = 1;
  const ViewModel one = build_view_model(exact, flat_thirsty_many(), kFlatNow, initial_ui_state(c));
  CHECK(one.hud.items.size() == 1);
  CHECK(one.hud.overflow == 5);
}

TEST_CASE("HUD is empty when all is well, and independent of the layer") {
  const Config c = flat_config();
  CHECK(build_view_model(c, flat_all_ok(), kFlatNow, initial_ui_state(c)).hud.items.empty());
  Config no_soil = c;
  no_soil.layers[index_of(Layer::Soil)].enabled = false;
  for (Layer l : {Layer::Temperature, Layer::Humidity, Layer::Air}) {
    const ViewModel vm = build_view_model(no_soil, flat_thirsty_many(), kFlatNow, state(l));
    CHECK(vm.hud.items.size() == 4);
    CHECK(vm.hud.overflow == 2);
  }
}

TEST_CASE("HUD: ties broken danger first, then thirst, then config order") {
  Config c = flat_config();
  c.hud.max_items = 20;
  Readings r = flat_all_ok();
  // basil (dry 0, wet 55) and lavender (dry 0, wet 50) both bone dry: thirst 1.0 each.
  r.by_entity["sensor.example_basil_moisture"].value = 0;
  r.by_entity["sensor.example_lavender_moisture"].value = 0;
  // snake (3..40) at thirst exactly 1.0 too, and monstera at Warn.
  r.by_entity["sensor.example_snake_moisture"].value = 3;
  r.by_entity["sensor.example_monstera_moisture"].value = 5 + 55 * 0.2;  // thirst 0.8: Warn
  const ViewModel vm = build_view_model(c, r, kFlatNow, initial_ui_state(c));
  CHECK(hud_ids(c, vm) ==
        std::vector<std::string>{"plant_basil", "plant_snake", "plant_lavender", "plant_monstera"});
  // Config order decides the tie: move lavender to the front of the pin list.
  Config moved = c;
  const std::size_t lav = pin_index(moved, "plant_lavender");
  Pin lavender = moved.pins[lav];
  moved.pins.erase(moved.pins.begin() + static_cast<std::ptrdiff_t>(lav));
  moved.pins.insert(moved.pins.begin(), lavender);
  const ViewModel vm2 = build_view_model(moved, r, kFlatNow, initial_ui_state(moved));
  CHECK(hud_ids(moved, vm2) ==
        std::vector<std::string>{"plant_lavender", "plant_basil", "plant_snake", "plant_monstera"});
}

TEST_CASE("HUD: thresholds exactly, stale plants, too-wet plants") {
  Config c = flat_config();
  Readings r = flat_all_ok();
  // basil: dry 0, wet 55; default soil band warn_above 0.7, danger_above 0.9.
  auto basil_at = [&](double thirst_value) {
    r.by_entity["sensor.example_basil_moisture"].value = 55.0 * (1.0 - thirst_value);
    return build_view_model(c, r, kFlatNow, initial_ui_state(c));
  };
  CHECK(basil_at(0.7).hud.items.empty());  // exactly warn_above: Ok
  auto vm = basil_at(0.71);
  REQUIRE(vm.hud.items.size() == 1);
  CHECK(vm.hud.items[0].severity == Severity::Warn);
  vm = basil_at(0.9);  // exactly danger_above: Warn
  CHECK(vm.hud.items[0].severity == Severity::Warn);
  vm = basil_at(0.91);
  CHECK(vm.hud.items[0].severity == Severity::Danger);

  // Stale plants are not listed, however dry their last reading.
  const ViewModel stale = build_view_model(c, flat_stale_sensors(), kFlatNow, initial_ui_state(c));
  CHECK(stale.hud.items.empty());
  Readings old = flat_thirsty_many();
  old.by_entity["sensor.example_basil_moisture"].last_updated = kFlatNow - 10801;
  const ViewModel vm_old = build_view_model(c, old, kFlatNow, initial_ui_state(c));
  for (const auto& item : vm_old.hud.items) CHECK(c.pins[item.pin_index].id != "plant_basil");
  CHECK(vm_old.hud.items.size() + vm_old.hud.overflow == 5);

  // A too-wet plant (Warn on the wet side) is not listed.
  Config wet = flat_config();
  wet.layers[index_of(Layer::Soil)].band.warn_below = 0.05;
  wet.layers[index_of(Layer::Soil)].band.danger_below = 0.01;
  Readings soaked = flat_all_ok();
  soaked.by_entity["sensor.example_basil_moisture"].value = 55;  // thirst 0: Danger, wet side
  const ViewModel vm_wet = build_view_model(wet, soaked, kFlatNow, initial_ui_state(wet));
  CHECK(pin_view(vm_wet, wet, "plant_basil").primary.severity == Severity::Danger);
  CHECK(vm_wet.hud.items.empty());
}

TEST_CASE("HUD: dry side means past comfort_max even with no warn_above (R2)") {
  Config c = flat_config();
  Band& soil = c.layers[index_of(Layer::Soil)].band;
  soil.warn_above.reset();  // only a danger edge on the dry side
  Readings r = flat_all_ok();
  r.by_entity["sensor.example_basil_moisture"].value = 0;            // thirst 1 > danger_above 0.9
  r.by_entity["sensor.example_lavender_moisture"].value = 50 * 0.2;  // thirst 0.8: Ok now
  const ViewModel vm = build_view_model(c, r, kFlatNow, initial_ui_state(c));
  CHECK(hud_ids(c, vm) == std::vector<std::string>{"plant_basil"});
}

TEST_CASE("HUD: soil pins never appear") {
  Config c = flat_config();
  Readings r = flat_all_ok();
  r.by_entity["sensor.example_planter_moisture"].value = 1;  // bone dry
  const ViewModel vm = build_view_model(c, r, kFlatNow, initial_ui_state(c));
  CHECK(pin_view(vm, c, "soil_planter").primary.severity == Severity::Danger);
  CHECK(vm.hud.items.empty());
}

// --- build_view_model: pins, rooms, tabs ----------------------------------------------------

TEST_CASE("pins: positions, primary, on_active_layer and active values") {
  const Config c = flat_config();
  const ViewModel vm = build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Humidity));
  REQUIRE(vm.pins.size() == c.pins.size());
  for (std::size_t i = 0; i < vm.pins.size(); ++i) CHECK(vm.pins[i].pin_index == i);

  const PinView& living = pin_view(vm, c, "clim_living");
  CHECK(living.position.x == doctest::Approx(0.2083 * 960));
  CHECK(living.position.y == doctest::Approx(0.3 * 800));
  CHECK(living.primary.role == Role::Temperature);
  CHECK(living.on_active_layer);  // its humidity binding feeds the humidity layer
  CHECK(*living.active_value == 48.0);

  const PinView& kitchen = pin_view(vm, c, "clim_kitchen");  // temperature only
  CHECK_FALSE(kitchen.on_active_layer);
  CHECK_FALSE(kitchen.active_value.has_value());
  CHECK(kitchen.active_severity == kitchen.primary.severity);

  const PinView& fiddle = pin_view(vm, c, "plant_fiddle");  // humidity is display-only
  CHECK_FALSE(fiddle.on_active_layer);

  const ViewModel soil = build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Soil));
  const PinView& basil = pin_view(soil, c, "plant_basil");
  CHECK(basil.on_active_layer);
  CHECK(*basil.active_value == doctest::Approx(1.0 - 30.0 / 55.0));
}

TEST_CASE("pins: a stale binding on the active layer has no value and Stale severity") {
  const Config c = flat_config();
  const ViewModel vm = build_view_model(c, flat_stale_sensors(), kFlatNow, state(Layer::Humidity));
  const PinView& bath = pin_view(vm, c, "hum_bathroom");
  CHECK(bath.on_active_layer);
  CHECK_FALSE(bath.active_value.has_value());
  CHECK(bath.active_severity == Severity::Stale);
  // On the temperature layer the same pin's temperature binding is fresh.
  const ViewModel t =
      build_view_model(c, flat_stale_sensors(), kFlatNow, state(Layer::Temperature));
  CHECK(pin_view(t, c, "hum_bathroom").active_severity == Severity::Ok);
  CHECK(pin_view(t, c, "hum_bathroom").primary.severity == Severity::Stale);
}

TEST_CASE("rooms pulse at the worst active-layer severity; outdoor and stale never pulse") {
  const Config c = flat_config();
  const ViewModel vm = build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Humidity));
  CHECK(vm.rooms[room_index(c, "bathroom")].pulse == Severity::Danger);  // 82 > 75
  CHECK(vm.rooms[room_index(c, "bedroom")].pulse == Severity::Warn);     // 66 > 65
  CHECK(vm.rooms[room_index(c, "balcony")].pulse == Severity::Ok);       // 92, outdoor
  CHECK(vm.rooms[room_index(c, "living")].pulse == Severity::Ok);
  // Active layer only (D-008): on temperature nothing pulses.
  const ViewModel t =
      build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Temperature));
  for (const auto& rv : t.rooms) CHECK(rv.pulse == Severity::Ok);
  // Stale counts as Ok.
  const ViewModel s = build_view_model(c, flat_stale_sensors(), kFlatNow, state(Layer::Humidity));
  for (const auto& rv : s.rooms) CHECK(rv.pulse == Severity::Ok);
}

TEST_CASE("rooms: worst of several pins, danger beats warn; soil layer pulses on thirst") {
  const Config c = flat_config();
  Readings r = flat_humid_bathroom();
  r.by_entity["sensor.example_bedroom_humidity"].value = 80;  // bedroom now danger
  const ViewModel vm = build_view_model(c, r, kFlatNow, state(Layer::Humidity));
  CHECK(vm.rooms[room_index(c, "bedroom")].pulse == Severity::Danger);

  const ViewModel soil = build_view_model(c, flat_thirsty_many(), kFlatNow, state(Layer::Soil));
  CHECK(soil.rooms[room_index(c, "kitchen")].pulse == Severity::Danger);  // basil
  CHECK(soil.rooms[room_index(c, "bathroom")].pulse == Severity::Warn);   // fern
  CHECK(soil.rooms[room_index(c, "balcony")].pulse == Severity::Danger);  // lavender, outdoor
}

TEST_CASE("disabled active layer: default layer shown instead; tabs list enabled layers") {
  Config c = flat_config();
  c.layers[index_of(Layer::Humidity)].enabled = false;
  const ViewModel vm =
      build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Humidity, "plant_basil"));
  CHECK(vm.active_layer == Layer::Temperature);
  CHECK(vm.tabs == std::vector<Layer>{Layer::Temperature, Layer::Soil, Layer::Air});
  for (const auto& rv : vm.rooms) CHECK(rv.pulse == Severity::Ok);
  REQUIRE(vm.overlay.has_value());
  CHECK(c.pins[vm.overlay->pin_index].id == "plant_basil");

  const ViewModel all =
      build_view_model(flat_config(), flat_all_ok(), kFlatNow, initial_ui_state(flat_config()));
  CHECK(all.tabs ==
        std::vector<Layer>{Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air});
}

// --- build_view_model: overlay ---------------------------------------------------------------

TEST_CASE("overlay: rows in Role order, predicted_dry_at hidden, units and timestamps") {
  const Config c = flat_config();
  // fiddle binds moisture, temperature, humidity, battery in that order.
  const ViewModel vm =
      build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Soil, "plant_fiddle"));
  REQUIRE(vm.overlay.has_value());
  const auto& rows = vm.overlay->rows;
  REQUIRE(rows.size() == 4);
  CHECK(rows[0].assessment.role == Role::Temperature);
  CHECK(rows[1].assessment.role == Role::Humidity);
  CHECK(rows[2].assessment.role == Role::Moisture);
  CHECK(rows[3].assessment.role == Role::Battery);
  CHECK(rows[0].unit == "°C");
  CHECK(*rows[0].last_updated == kFlatNow - 300);
  CHECK(*rows[2].assessment.value == doctest::Approx(1.0 - 35.0 / 60.0));

  const ViewModel basil =
      build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Soil, "plant_basil"));
  REQUIRE(basil.overlay->rows.size() == 1);  // predicted_dry_at not shown
  CHECK(basil.overlay->rows[0].assessment.role == Role::Moisture);
}

TEST_CASE("overlay: a missing reading has a row with no unit or timestamp") {
  const Config c = flat_config();
  const ViewModel vm =
      build_view_model(c, flat_stale_sensors(), kFlatNow, state(Layer::Air, "air_bedroom"));
  REQUIRE(vm.overlay);
  const auto& co2 = vm.overlay->rows[0];
  CHECK(co2.assessment.role == Role::Co2);
  CHECK(co2.assessment.freshness == Freshness::Missing);
  CHECK(co2.unit.empty());
  CHECK_FALSE(co2.last_updated.has_value());
}

TEST_CASE("overlay: none requested, or an unknown id, gives no overlay") {
  const Config c = flat_config();
  CHECK_FALSE(build_view_model(c, flat_all_ok(), kFlatNow, initial_ui_state(c)).overlay);
  CHECK_FALSE(build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Soil, "gone")).overlay);
}

TEST_CASE("build_view_model is deterministic") {
  const Config c = flat_config();
  const ViewModel a = build_view_model(c, flat_thirsty_many(), kFlatNow, state(Layer::Soil));
  const ViewModel b = build_view_model(c, flat_thirsty_many(), kFlatNow, state(Layer::Soil));
  CHECK(hud_ids(c, a) == hud_ids(c, b));
  CHECK(a.hud.overflow == b.hud.overflow);
}

// --- heat_samples ----------------------------------------------------------------------------

TEST_CASE("heat_samples: one per fresh feeding binding, with room index and value") {
  const Config c = flat_config();
  const auto temp = heat_samples(c, flat_all_ok(), kFlatNow, Layer::Temperature);
  // Temperature feeders: 7 climate pins (hum_bathroom's temperature counts); plants' do not.
  CHECK(temp.size() == 7);
  CHECK(temp[0].room_index == room_index(c, "living"));
  CHECK(temp[0].value == doctest::Approx(21.5));

  const auto stale = heat_samples(c, flat_stale_sensors(), kFlatNow, Layer::Temperature);
  CHECK(stale.size() == 5);  // kitchen and study too old

  const auto soil = heat_samples(c, flat_all_ok(), kFlatNow, Layer::Soil);
  CHECK(soil.size() == 9);  // 8 plants + the planter
  const auto air = heat_samples(c, flat_stale_sensors(), kFlatNow, Layer::Air);
  CHECK(air.size() == 1);  // bedroom CO2 missing
}

TEST_CASE("heat_samples: disabled layer is empty; unknown room skipped") {
  Config c = flat_config();
  c.layers[index_of(Layer::Air)].enabled = false;
  CHECK(heat_samples(c, flat_all_ok(), kFlatNow, Layer::Air).empty());
  Config d = flat_config();
  d.pins[0].room_id = "nowhere";
  CHECK(heat_samples(d, flat_all_ok(), kFlatNow, Layer::Temperature).size() == 6);
}

TEST_CASE("HUD: large tied groups keep config order (the sort must be stable)") {
  // Sixty plants in two interleaved tied groups (thirst 1.0 and 0.95, all Danger). Within each
  // group config order alone decides. Small or all-equal inputs can come out in order even from
  // an unstable sort; this one does not (checked against std::sort while writing it).
  Config c;
  c.rooms.push_back(Room{"room", "Room", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, false});
  c.hud.max_items = 20;
  Readings r;
  for (int i = 0; i < 60; ++i) {
    Pin p;
    p.id = "plant_" + std::to_string(i);
    p.kind = PinKind::Plant;
    p.name = p.id;
    p.position = {0.015 * i + 0.01, 0.5};
    p.room_id = "room";
    p.entities = {{Role::Moisture, "sensor.example_m" + std::to_string(i)}};
    p.calibration = Calibration{};
    c.pins.push_back(p);
    r.by_entity["sensor.example_m" + std::to_string(i)] =
        value_reading(i % 2 == 0 ? 0.0 : 5.0, kFlatNow);  // thirst 1.0 or 0.95
  }
  const ViewModel vm = build_view_model(c, r, kFlatNow, initial_ui_state(c));
  REQUIRE(vm.hud.items.size() == 20);
  CHECK(vm.hud.overflow == 40);
  // The first 20 are the even (thirst 1.0) plants, in config order.
  for (std::size_t i = 0; i < 20; ++i) CHECK(vm.hud.items[i].pin_index == 2 * i);
}

// --- out-parameter forms ---------------------------------------------------------------------

TEST_CASE("build_view_model into a reused ViewModel equals a fresh build") {
  const Config c = flat_config();
  ViewModel reused;
  // Warm it with a different state (overlay open, other layer), then rebuild.
  build_view_model(c, flat_thirsty_many(), kFlatNow, state(Layer::Soil, "plant_fiddle"), reused);
  build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Humidity, "plant_basil"),
                   reused);
  const ViewModel fresh =
      build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Humidity, "plant_basil"));
  CHECK(reused.active_layer == fresh.active_layer);
  CHECK(reused.tabs == fresh.tabs);
  REQUIRE(reused.pins.size() == fresh.pins.size());
  for (std::size_t i = 0; i < fresh.pins.size(); ++i) {
    CHECK(reused.pins[i].active_severity == fresh.pins[i].active_severity);
    CHECK(reused.pins[i].active_value == fresh.pins[i].active_value);
    CHECK(reused.pins[i].on_active_layer == fresh.pins[i].on_active_layer);
  }
  for (std::size_t r = 0; r < fresh.rooms.size(); ++r) {
    CHECK(reused.rooms[r].pulse == fresh.rooms[r].pulse);
  }
  CHECK(hud_ids(c, reused) == hud_ids(c, fresh));
  CHECK(reused.hud.overflow == fresh.hud.overflow);
  REQUIRE(reused.overlay.has_value());
  CHECK(reused.overlay->rows.size() == 1);  // fiddle's four rows were cut back to basil's one
  CHECK(reused.overlay->pin_index == fresh.overlay->pin_index);
  // Closing the overlay clears it.
  build_view_model(c, flat_humid_bathroom(), kFlatNow, state(Layer::Humidity), reused);
  CHECK_FALSE(reused.overlay.has_value());
}

TEST_CASE("overlay row reuse: a row without a reading loses the previous unit and time") {
  const Config c = flat_config();
  ViewModel vm;
  build_view_model(c, flat_all_ok(), kFlatNow, state(Layer::Air, "air_bedroom"), vm);
  REQUIRE(vm.overlay);
  CHECK(vm.overlay->rows[0].unit == "ppm");
  build_view_model(c, flat_stale_sensors(), kFlatNow, state(Layer::Air, "air_bedroom"), vm);
  CHECK(vm.overlay->rows[0].unit.empty());
  CHECK_FALSE(vm.overlay->rows[0].last_updated.has_value());
}

TEST_CASE("heat_samples into a reused vector clears it first") {
  const Config c = flat_config();
  std::vector<HeatSample> out(50);
  heat_samples(c, flat_all_ok(), kFlatNow, Layer::Air, out);
  CHECK(out.size() == 2);
  heat_samples(c, flat_all_ok(), kFlatNow, Layer::Air, out);
  CHECK(out.size() == 2);
  CHECK(out[0].value == doctest::Approx(620.0F));
}

TEST_CASE("disabled soil layer: plants unjudged on their pins, HUD still lists them (D-020)") {
  Config c = flat_config();
  c.layers[index_of(Layer::Soil)].enabled = false;
  const ViewModel vm = build_view_model(c, flat_thirsty_many(), kFlatNow, initial_ui_state(c));
  CHECK(pin_view(vm, c, "plant_basil").primary.severity == Severity::Ok);
  CHECK(vm.hud.items.size() == 4);
  CHECK(vm.hud.items[0].severity == Severity::Danger);
}
