// scenarios.h — houses and photo providers shared by the host tests that drive the real Screen
// through App (perf_proxy_test.cpp, memory_budget_test.cpp).
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "app.h"
#include "hearthmap/config.h"
#include "hearthmap/readings.h"
#include "hearthmap/ui/ports.h"
#include "json_config.h"
#include "lvgl.h"

namespace hearthmap::host::test {

inline std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
// lv_image_header_t is a set of bit-fields; GCC's -Wconversion flags every write to them.
#pragma GCC diagnostic ignored "-Wconversion"
#endif
/// Plant photos as the device has them: RGB565, pre-sized for every disc (D-039).
class SizedPhotos final : public ui::ImageProvider {
 public:
  const lv_image_dsc_t* plant_photo(std::string_view /*id*/) const override { return get(128); }
  const lv_image_dsc_t* plant_photo_sized(std::string_view /*id*/, int diameter) const override {
    return get(diameter);
  }

 private:
  struct Img {
    std::vector<std::uint16_t> px;
    lv_image_dsc_t dsc{};
  };
  const lv_image_dsc_t* get(int d) const {
    auto& slot = cache_[d];
    if (!slot) {
      slot = std::make_unique<Img>();
      slot->px.assign(static_cast<std::size_t>(d) * static_cast<std::size_t>(d), 0x3D4A);
      slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      slot->dsc.header.w = static_cast<std::uint32_t>(d);
      slot->dsc.header.h = static_cast<std::uint32_t>(d);
      slot->dsc.header.stride = static_cast<std::uint32_t>(d) * 2U;
      slot->dsc.data_size = static_cast<std::uint32_t>(slot->px.size() * 2U);
      slot->dsc.data = reinterpret_cast<const std::uint8_t*>(slot->px.data());
    }
    return &slot->dsc;
  }
  mutable std::map<int, std::unique_ptr<Img>> cache_;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/// A fictional house shaped like the panel's first real install (the same shape as
/// components/hearthmap/tests/test_cpp.py's panel-like config): 18 rooms on a 6 x 3 grid, some
/// L-shaped, the last 4 outdoor; a 920 x 736 map below the top bar; a 131 x 105 grid at power 3;
/// 22 pins of which 14 are plants with photos, 9 of them bone dry.
struct Scenario {
  Config config;
  Readings readings;
  Timestamp now = 0;
};

inline Scenario panel_like() {
  Scenario s;
  Config& c = s.config;
  c.canvas.map_area = CanvasRect{0, 64, 920, 736};
  c.heat.grid_cols = 131;
  c.heat.grid_rows = 105;
  c.heat.idw_power = 3.0;
  for (int i = 0; i < 18; ++i) {
    const double x0 = (i % 6) / 6.0, y0 = (i / 6) / 3.0;
    const double x1 = x0 + 1.0 / 6.0, y1 = y0 + 1.0 / 3.0;
    Room r;
    r.id = "room_" + std::to_string(i);
    r.name = "Room " + std::to_string(i);
    r.outdoor = i >= 14;
    if (i % 5 == 2) {
      const double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
      r.polygon = {{x0, y0}, {x1, y0}, {x1, my}, {mx, my}, {mx, y1}, {x0, y1}};
    } else {
      r.polygon = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    c.rooms.push_back(r);
  }
  s.now = 1'000'000;
  auto value = [&](const std::string& entity, double v) {
    Reading r;
    r.state = ReadingState::Value;
    r.value = v;
    r.last_updated = s.now - 60;
    s.readings.by_entity[entity] = r;
  };
  for (int k = 0; k < 22; ++k) {
    const int room = k % 14;  // indoor rooms only
    const bool plant = k >= 8;
    Pin p;
    p.id = "pin_" + std::to_string(k);
    p.room_id = "room_" + std::to_string(room);
    // Plants sit in the lower half of their room, sensors in the upper; never on a wall.
    const double cx = (room % 6 + 0.3 + 0.4 * ((k / 14) % 2)) / 6.0;
    const double cy = (room / 6 + (plant ? 0.62 : 0.25)) / 3.0;
    p.position = MapPoint{cx, cy};
    const std::string entity = "sensor.example_house_" + std::to_string(k);
    if (plant) {
      p.kind = PinKind::Plant;
      p.name = "Plant " + std::to_string(k);
      p.photo = "photo_" + std::to_string(k);
      p.calibration = Calibration{0.0, 100.0, std::nullopt, std::nullopt};
      p.entities = {{Role::Moisture, entity}};
      value(entity, k < 17 ? 2.0 : 60.0);  // pins 8..16 bone dry: nine in danger
    } else {
      p.kind = PinKind::Temperature;
      p.name = "Sensor " + std::to_string(k);
      p.entities = {{Role::Temperature, entity}};
      // Some rooms too hot (danger) or warm (warn), so rooms pulse on the temperature layer too.
      value(entity, k % 3 == 0 ? 33.0 : (k % 3 == 1 ? 28.0 : 21.0));
    }
    c.pins.push_back(p);
  }
  return s;
}

/// panel_like() with its plants moved next to walls and corners (14 to 26 px from a wall), as in
/// an install whose rooms share long walls with pins near their edges: every danger ring's tile
/// then crosses outline cells of pulsing neighbours, and rings near one corner overlap.
inline Scenario edge_heavy() {
  Scenario s = panel_like();
  for (std::size_t k = 8; k < s.config.pins.size(); ++k) {
    Pin& p = s.config.pins[k];
    const int room = static_cast<int>(k % 14);
    const double w = 1.0 / 6.0, h = 1.0 / 3.0;
    const double x0 = (room % 6) * w, y0 = (room / 6) * h;
    // Alternately near the left wall, near the bottom-right corner, and near the top wall.
    switch (k % 3) {
      case 0: p.position = MapPoint{x0 + 0.12 * w, y0 + 0.55 * h}; break;
      case 1: p.position = MapPoint{x0 + 0.86 * w, y0 + 0.92 * h}; break;
      default: p.position = MapPoint{x0 + 0.5 * w, y0 + 0.06 * h}; break;
    }
  }
  return s;
}

inline Scenario example_thirsty() {
  const std::string dir = std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/";
  Scenario s;
  s.config = *JsonConfigSource(slurp(dir + "config.json")).load().config;
  const ReadingsFixture fx = *parse_readings(slurp(dir + "readings/thirsty-many.json")).fixture;
  s.readings = fx.readings;
  s.now = fx.now;
  return s;
}

}  // namespace hearthmap::host::test
