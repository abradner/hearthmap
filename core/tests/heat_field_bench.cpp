// Heat-field benchmark (host only). Prints wall-clock timings for build_room_mask() and
// compute_heat_field() on the example flat's grid and on a 20-room, 60-sensor house at the UI
// spec's resolution (about one cell per 7 px over a 920 x 736 map). Asserts nothing about time:
// CI runs a Debug build on shared runners. Exits non-zero only if a field has the wrong shape.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "heat_fixtures.h"

using namespace hearthmap;

namespace {

using Clock = std::chrono::steady_clock;

double microseconds_since(Clock::time_point start, int iterations) {
  const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start);
  return elapsed.count() / iterations;
}

// Runs one case; returns false if the output has the wrong shape.
bool run(const char* name, const Config& config, const std::vector<HeatSample>& samples,
         int iterations) {
  auto start = Clock::now();
  RoomMask mask;
  for (int i = 0; i < iterations; ++i) mask = build_room_mask(config);
  const double mask_us = microseconds_since(start, iterations);

  start = Clock::now();
  HeatField field;
  for (int i = 0; i < iterations; ++i) field = compute_heat_field(config, mask, samples);
  const double field_us = microseconds_since(start, iterations);

  std::size_t values = 0;
  for (const HeatCell& cell : field.cells) values += cell.state == CellState::Value ? 1U : 0U;
  std::printf("%-34s grid %3dx%-3d rooms %2zu samples %2zu  mask %9.1f us  field %9.1f us\n", name,
              static_cast<int>(field.cols), static_cast<int>(field.rows), config.rooms.size(),
              samples.size(), mask_us, field_us);
  const auto expected = static_cast<std::size_t>(config.heat.grid_cols) *
                        static_cast<std::size_t>(config.heat.grid_rows);
  return field.cells.size() == expected && values > 0;
}

}  // namespace

int main() {
  bool ok = true;
  const Config flat = fixtures::example_flat_config();
  const auto flat_samples = fixtures::example_flat_temperatures();
  ok = run("example flat (config grid)", flat, flat_samples, 200) && ok;

  Config flat_fine = flat;
  flat_fine.heat.grid_cols = 131;
  flat_fine.heat.grid_rows = 105;
  ok = run("example flat (1 cell / 7 px)", flat_fine, flat_samples, 50) && ok;

  const auto house = fixtures::twenty_rooms_sixty_sensors();
  ok = run("20 rooms, 60 sensors (1 cell / 7 px)", house.config, house.samples, 50) && ok;

  auto house_max = fixtures::twenty_rooms_sixty_sensors(256, 256);
  ok = run("20 rooms, 60 sensors (256x256 max)", house_max.config, house_max.samples, 10) && ok;

  house_max.config.heat.idw_power = 2.5;  // non-integer: the std::pow path
  ok = run("same, idw_power 2.5 (std::pow)", house_max.config, house_max.samples, 10) && ok;
  return ok ? 0 : 1;
}
