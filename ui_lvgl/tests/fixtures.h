// Hand-built configs and view models for ui_lvgl's tests: no core logic is involved, so these
// tests check drawing and diffing only.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/view_model.h"

namespace hearthmap::test {

// Map area 864 x 720 at (40, 76). Three rooms across the top half: west (indoor), east (indoor),
// and a balcony across the bottom (outdoor). Pins: 0 temperature (west), 1 plant (west,
// photo "fern"), 2 plant (east, no photo), 3 humidity (east), 4 soil (balcony).
inline Config small_config() {
  Config c;
  c.canvas.map_area = CanvasRect{40, 76, 864, 720};
  c.rooms = {
      Room{"west", "West", {{0.0, 0.0}, {0.5, 0.0}, {0.5, 0.5}, {0.0, 0.5}}, false},
      Room{"east", "East", {{0.5, 0.0}, {1.0, 0.0}, {1.0, 0.5}, {0.5, 0.5}}, false},
      Room{"balcony", "Balcony", {{0.0, 0.5}, {1.0, 0.5}, {1.0, 1.0}, {0.0, 1.0}}, true},
  };
  auto pin = [](std::string id, PinKind kind, std::string name, MapPoint pos, std::string room,
                std::vector<EntityBinding> e) {
    Pin p;
    p.id = std::move(id);
    p.kind = kind;
    p.name = std::move(name);
    p.position = pos;
    p.room_id = std::move(room);
    p.entities = std::move(e);
    return p;
  };
  c.pins.push_back(pin(
      "t_west", PinKind::Temperature, "West", {0.25, 0.25}, "west",
      {{Role::Temperature, "sensor.example_west_t"}, {Role::Humidity, "sensor.example_west_h"}}));
  Pin fern =
      pin("plant_fern", PinKind::Plant, "Fern", {0.1, 0.1}, "west",
          {{Role::Moisture, "sensor.example_fern_m"}, {Role::Battery, "sensor.example_fern_b"}});
  fern.photo = "fern";
  fern.calibration = Calibration{0.0, 100.0, std::nullopt, std::nullopt};
  c.pins.push_back(fern);
  Pin ivy = pin("plant_ivy", PinKind::Plant, "Ivy", {0.75, 0.1}, "east",
                {{Role::Moisture, "sensor.example_ivy_m"}});
  ivy.calibration = Calibration{0.0, 100.0, std::nullopt, std::nullopt};
  c.pins.push_back(ivy);
  c.pins.push_back(pin("h_east", PinKind::Humidity, "East", {0.75, 0.3}, "east",
                       {{Role::Humidity, "sensor.example_east_h"}}));
  Pin soil = pin("soil_box", PinKind::Soil, "Box", {0.5, 0.75}, "balcony",
                 {{Role::Moisture, "sensor.example_box_m"}});
  soil.calibration = Calibration{0.0, 100.0, std::nullopt, std::nullopt};
  c.pins.push_back(soil);
  c.hud.max_items = 2;
  c.heat.grid_cols = 48;
  c.heat.grid_rows = 40;
  return c;
}

inline Assessment fresh(Role role, double value, Severity sev, std::optional<Band> band = {}) {
  Assessment a;
  a.role = role;
  a.freshness = Freshness::Fresh;
  a.value = value;
  a.raw = value;
  a.severity = sev;
  a.band = band;
  return a;
}

inline Assessment stale(Role role, Freshness why = Freshness::Missing) {
  Assessment a;
  a.role = role;
  a.freshness = why;
  a.severity = Severity::Stale;
  return a;
}

inline PinView pin_view(const Config& c, std::size_t i, Assessment primary, bool on_layer,
                        std::optional<double> active_value, Severity active_sev) {
  PinView pv;
  pv.pin_index = i;
  pv.position = to_canvas(c.canvas.map_area, c.pins[i].position);
  pv.primary = std::move(primary);
  pv.on_active_layer = on_layer;
  pv.active_value = active_value;
  pv.active_severity = active_sev;
  return pv;
}

/// Temperature layer, everything fine; the ivy is thirsty (warn) and the fern dry (danger).
inline ViewModel temperature_vm(const Config& c) {
  ViewModel vm;
  vm.active_layer = Layer::Temperature;
  vm.tabs = {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air};
  const Band soil = default_band(Layer::Soil);
  vm.pins.push_back(
      pin_view(c, 0, fresh(Role::Temperature, 22.6, Severity::Ok), true, 22.6, Severity::Ok));
  vm.pins.push_back(pin_view(c, 1, fresh(Role::Moisture, 0.95, Severity::Danger, soil), false,
                             std::nullopt, Severity::Danger));
  vm.pins.push_back(pin_view(c, 2, fresh(Role::Moisture, 0.8, Severity::Warn, soil), false,
                             std::nullopt, Severity::Warn));
  vm.pins.push_back(
      pin_view(c, 3, fresh(Role::Humidity, 50.0, Severity::Ok), false, std::nullopt, Severity::Ok));
  vm.pins.push_back(pin_view(c, 4, fresh(Role::Moisture, 0.3, Severity::Ok, soil), false,
                             std::nullopt, Severity::Ok));
  for (std::size_t r = 0; r < c.rooms.size(); ++r) vm.rooms.push_back(RoomView{r, Severity::Ok});
  vm.hud.items = {HudItem{1, 0.95, Severity::Danger}, HudItem{2, 0.8, Severity::Warn}};
  vm.hud.overflow = 0;
  return vm;
}

inline OverlayRow row(Assessment a, std::string unit, std::optional<Timestamp> t) {
  OverlayRow r;
  r.assessment = std::move(a);
  r.unit = std::move(unit);
  r.last_updated = t;
  return r;
}

}  // namespace hearthmap::test
