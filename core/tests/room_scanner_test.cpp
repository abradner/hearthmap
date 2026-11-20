// RoomScanner and the scanline build_room_mask(): exactly point_in_polygon() at every centre.
//
// The scanline is a performance change (docs/decisions.md D-038), so the contract it must keep is
// equality with the per-point answer, bit for bit, including the hard cases: centres exactly on
// a vertex or an edge, shared diagonal walls, concave and comb-shaped rooms, overlapping rooms
// (first wins), degenerate polygons and grids that do not divide the map evenly.
#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/geometry.h"
#include "hearthmap/heat_field.h"
#include "heat_fixtures.h"

using namespace hearthmap;

namespace {

// The reference: point_in_polygon() at each centre, rooms in config order, first wins.
std::vector<std::int16_t> brute_force(const Config& config, std::int32_t cols, std::int32_t rows) {
  std::vector<std::int16_t> out(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows),
                                RoomMask::kOutside);
  for (std::int32_t r = 0; r < rows; ++r) {
    for (std::int32_t c = 0; c < cols; ++c) {
      const MapPoint p{(c + 0.5) / cols, (r + 0.5) / rows};
      for (std::size_t i = 0; i < config.rooms.size(); ++i) {
        if (point_in_polygon(config.rooms[i].polygon, p)) {
          out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
              static_cast<std::size_t>(c)] = static_cast<std::int16_t>(i);
          break;
        }
      }
    }
  }
  return out;
}

std::vector<std::int16_t> scanned(const Config& config, std::int32_t cols, std::int32_t rows) {
  RoomScanner scanner(config);
  std::vector<std::int16_t> out(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows),
                                12345);
  for (std::int32_t r = 0; r < rows; ++r) {
    scanner.scan_row(cols, rows, r,
                     &out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols)]);
  }
  return out;
}

void check_equal(const Config& config, std::int32_t cols, std::int32_t rows) {
  const std::vector<std::int16_t> expected = brute_force(config, cols, rows);
  CHECK(scanned(config, cols, rows) == expected);
  Config grid = config;
  grid.heat.grid_cols = cols;
  grid.heat.grid_rows = rows;
  CHECK(build_room_mask(grid).room_index == expected);
}

Room room(std::vector<MapPoint> polygon) {
  Room r;
  r.id = "r";
  r.name = "R";
  r.polygon = std::move(polygon);
  return r;
}

// A comb: a bar along the bottom and `teeth` teeth reaching up, inside [x0, x1]. With 16 teeth
// it has 64 vertices (the schema's maximum), and every row through the teeth crosses 32 edges.
Polygon comb(double x0, double x1, int teeth) {
  Polygon p;
  const double w = (x1 - x0) / (2 * teeth - 1);
  p.push_back({x0, 1.0});
  for (int t = 0; t < teeth; ++t) {
    const double l = x0 + 2 * t * w;
    p.push_back({l, 0.05});
    p.push_back({l + w, 0.05});
    if (t + 1 < teeth) {
      p.push_back({l + w, 0.9});
      p.push_back({l + 2 * w, 0.9});
    }
  }
  p.push_back({x1, 1.0});
  return p;
}

}  // namespace

TEST_CASE("scanline equals point_in_polygon on the example flat, several grids") {
  const Config config = fixtures::example_flat_config();
  const std::pair<std::int32_t, std::int32_t> grids[] = {{48, 40}, {131, 105}, {7, 3},
                                                         {2, 2},   {256, 256}, {864, 720}};
  for (const auto& grid : grids) {
    CAPTURE(grid.first);
    CAPTURE(grid.second);
    check_equal(config, grid.first, grid.second);
  }
}

TEST_CASE("centres exactly on vertices and edges") {
  // Vertices at cell centres of a 10 x 10 grid ((c + 0.5) / 10): every tie point_in_polygon
  // resolves with its half-open rule is exercised.
  Config config;
  config.rooms.push_back(room({{0.05, 0.05}, {0.55, 0.05}, {0.55, 0.55}, {0.05, 0.55}}));
  config.rooms.push_back(room({{0.55, 0.05}, {0.95, 0.05}, {0.95, 0.55}, {0.55, 0.55}}));
  config.rooms.push_back(
      room({{0.05, 0.55}, {0.95, 0.95}, {0.05, 0.95}}));  // diagonal through centres
  config.rooms.push_back(room({{0.05, 0.55}, {0.95, 0.55}, {0.95, 0.95}}));  // shares that diagonal
  check_equal(config, 10, 10);
  check_equal(config, 20, 20);
  check_equal(config, 9, 11);
}

TEST_CASE("concave, comb-shaped and overlapping rooms; first room wins") {
  Config config;
  config.rooms.push_back(room(comb(0.0, 0.5, 16)));
  config.rooms.push_back(room(comb(0.25, 1.0, 16)));  // overlaps the first
  config.rooms.push_back(
      room({{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.5, 0.3}, {0.1, 0.9}}));  // concave
  REQUIRE(config.rooms[0].polygon.size() == 64);
  check_equal(config, 64, 64);
  check_equal(config, 256, 256);
}

TEST_CASE("degenerate polygons claim nothing; self-intersecting ones still match") {
  Config config;
  config.rooms.push_back(room({{0.1, 0.1}, {0.5, 0.5}, {0.9, 0.9}}));  // collinear
  config.rooms.push_back(room({{0.1, 0.1}, {0.9, 0.1}}));              // too few vertices
  config.rooms.push_back(room({{0.1, 0.1}, {0.9, 0.9}, {0.9, 0.1}, {0.1, 0.9}}));  // bow tie
  check_equal(config, 50, 50);
  const RoomMask mask = [&] {
    Config c = config;
    c.heat.grid_cols = 50;
    c.heat.grid_rows = 50;
    return build_room_mask(c);
  }();
  for (std::int16_t r : mask.room_index) CHECK(r != 0);  // the collinear room owns no cell
}

TEST_CASE("random polygons, including vertices on centres, match point_in_polygon") {
  std::mt19937 rng(20261121);
  std::uniform_int_distribution<int> grid_coord(0, 15);  // snapped to a 16-grid's centres
  std::uniform_real_distribution<double> any(0.0, 1.0);
  std::uniform_int_distribution<int> vertices(3, 12);
  for (int trial = 0; trial < 200; ++trial) {
    Config config;
    const int rooms = 1 + trial % 6;
    for (int r = 0; r < rooms; ++r) {
      Polygon p;
      const int n = vertices(rng);
      for (int k = 0; k < n; ++k) {
        if (trial % 2 == 0) {
          p.push_back({(grid_coord(rng) + 0.5) / 16, (grid_coord(rng) + 0.5) / 16});
        } else {
          p.push_back({any(rng), any(rng)});
        }
      }
      config.rooms.push_back(room(p));
    }
    CAPTURE(trial);
    check_equal(config, 16, 16);
    check_equal(config, 37, 23);
  }
}

TEST_CASE("RoomScanner ignores rows outside the grid and empty grids") {
  const Config config = fixtures::example_flat_config();
  RoomScanner scanner(config);
  std::vector<std::int16_t> row(8, 99);
  scanner.scan_row(8, 4, 4, row.data());   // row == rows
  scanner.scan_row(8, 4, -1, row.data());  // negative row
  scanner.scan_row(0, 4, 0, row.data());   // no columns
  for (std::int16_t v : row) CHECK(v == 99);
  CHECK(scanner.edge_count() > 0);
}

TEST_CASE("row-wise heat field equals compute_heat_field") {
  const auto house = fixtures::twenty_rooms_sixty_sensors();
  const RoomMask mask = build_room_mask(house.config);
  const HeatField whole = compute_heat_field(house.config, mask, house.samples);
  HeatField parts;
  REQUIRE(begin_heat_field(mask, parts));
  for (std::int32_t r = 0; r < mask.rows; r += 7) {
    compute_heat_field_rows(house.config, mask, house.samples, r, r + 7, parts);
  }
  REQUIRE(parts.cells.size() == whole.cells.size());
  for (std::size_t i = 0; i < whole.cells.size(); ++i) {
    CHECK(parts.cells[i].state == whole.cells[i].state);
    CHECK(parts.cells[i].value == whole.cells[i].value);
  }
  RoomMask bad;
  CHECK_FALSE(begin_heat_field(bad, parts));
  CHECK(parts.cells.empty());
  compute_heat_field_rows(house.config, mask, house.samples, 0, 1, parts);  // refused: no-op
  CHECK(parts.cells.empty());
}
