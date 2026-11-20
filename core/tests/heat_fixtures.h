// heat_fixtures.h — test-only configs and samples for the heat-field tests, benchmark and dump.
//
// example_flat_*() mirrors examples/flat/config.json (rooms, map area, heat settings) and the
// temperature readings in examples/flat/readings/all-ok.json, by hand: core has no JSON parser.
// If the example changes, change this too (the host renderer in PR 5 will read the JSON itself).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"

namespace hearthmap::fixtures {

inline Room rect_room(std::string id, double x0, double y0, double x1, double y1) {
  Room room;
  room.id = id;
  room.name = std::move(id);
  room.polygon = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  return room;
}

/// The fictional flat: seven rooms, the hallway an L-shaped (concave) hexagon.
inline Config example_flat_config() {
  Config config;
  config.canvas.map_area = CanvasRect{40, 76, 864, 720};
  config.rooms.push_back(rect_room("living", 0.0, 0.0, 0.4167, 0.6));
  config.rooms.push_back(rect_room("kitchen", 0.4167, 0.0, 0.6667, 0.4));
  config.rooms.push_back(rect_room("study", 0.6667, 0.0, 1.0, 0.4));
  Room hallway;
  hallway.id = "hallway";
  hallway.name = "Hallway";
  hallway.polygon = {{0.4167, 0.4}, {1.0, 0.4},    {1.0, 0.6},
                     {0.5833, 0.6}, {0.5833, 1.0}, {0.4167, 1.0}};
  config.rooms.push_back(hallway);
  config.rooms.push_back(rect_room("bathroom", 0.5833, 0.6, 0.7917, 1.0));
  config.rooms.push_back(rect_room("bedroom", 0.7917, 0.6, 1.0, 1.0));
  Room balcony = rect_room("balcony", 0.0, 0.6, 0.4167, 1.0);
  balcony.outdoor = true;
  config.rooms.push_back(balcony);
  config.heat.grid_cols = 48;
  config.heat.grid_rows = 40;
  config.heat.idw_power = 2.0;
  return config;
}

/// Temperature-layer samples for the example flat under all-ok.json: one per temperature pin
/// that feeds the layer (soil and plant pins' temperatures are display-only, D-004).
inline std::vector<HeatSample> example_flat_temperatures() {
  return {
      {{0.2083F, 0.3F}, 0, 21.5},   // clim_living
      {{0.5417F, 0.2F}, 1, 22.0},   // clim_kitchen
      {{0.8333F, 0.2F}, 2, 20.5},   // clim_study
      {{0.5F, 0.5F}, 3, 20.0},      // clim_hall
      {{0.6875F, 0.75F}, 4, 22.5},  // hum_bathroom (temperature role)
      {{0.8958F, 0.75F}, 5, 19.5},  // clim_bedroom
      {{0.2083F, 0.7F}, 6, 9.0F},   // clim_balcony (outdoor: no heat colour, D-006)
  };
}

/// A small deterministic generator (64-bit LCG, Knuth's constants) for synthetic fixtures.
class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  /// Uniform in [0, 1).
  double next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11U) / 9007199254740992.0;  // 2^53
  }

 private:
  std::uint64_t state_;
};

/// A synthetic house: a 5 x 4 grid of rooms with three samples each (20 rooms, 60 sensors),
/// on the full 920 x 736 map area at roughly one cell per 7 px.
struct SyntheticHouse {
  Config config;
  std::vector<HeatSample> samples;
};

inline SyntheticHouse twenty_rooms_sixty_sensors(std::int32_t cols = 131, std::int32_t rows = 105) {
  SyntheticHouse house;
  house.config.canvas.map_area = CanvasRect{0, 64, 920, 736};
  house.config.heat.grid_cols = cols;
  house.config.heat.grid_rows = rows;
  house.config.heat.idw_power = 2.0;
  Lcg rng(20261120);
  for (int ry = 0; ry < 4; ++ry) {
    for (int rx = 0; rx < 5; ++rx) {
      const double x0 = rx / 5.0;
      const double y0 = ry / 4.0;
      const double x1 = (rx + 1) / 5.0;
      const double y1 = (ry + 1) / 4.0;
      const auto index = static_cast<std::int16_t>(house.config.rooms.size());
      house.config.rooms.push_back(rect_room("r" + std::to_string(index), x0, y0, x1, y1));
      for (int s = 0; s < 3; ++s) {
        const double x = x0 + (0.1 + 0.8 * rng.next()) * (x1 - x0);
        const double y = y0 + (0.1 + 0.8 * rng.next()) * (y1 - y0);
        house.samples.push_back(
            {to_float(MapPoint{x, y}), index, static_cast<float>(15.0 + 15.0 * rng.next())});
      }
    }
  }
  return house;
}

}  // namespace hearthmap::fixtures
