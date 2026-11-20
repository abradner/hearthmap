// Tests for build_room_mask() and compute_heat_field() (heat_field.h, docs/decisions.md D-005).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "heat_fixtures.h"

using namespace hearthmap;
using fixtures::rect_room;

// Counts every allocation in this test binary, so the "no allocation once warm" contract
// (D-022) is checked against operator new itself rather than against capacity().
namespace {
std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
  ++g_allocations;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  std::abort();  // core and its tests are built without relying on bad_alloc
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}

namespace {

// Room index of the cell whose centre is nearest the map point (x, y).
std::int16_t room_at(const RoomMask& mask, double x, double y) {
  const auto c = static_cast<std::int32_t>(x * mask.cols);
  const auto r = static_cast<std::int32_t>(y * mask.rows);
  return mask.room_index[mask.index(c, r)];
}

const HeatCell& cell_at(const HeatField& field, std::int32_t c, std::int32_t r) {
  return field.cells[static_cast<std::size_t>(r) * static_cast<std::size_t>(field.cols) +
                     static_cast<std::size_t>(c)];
}

// Two side-by-side rooms, A = left half (index 0) and B = right half (index 1), on a square
// 100 x 100 px map area with a 10 x 10 grid, power 2.
Config two_rooms() {
  Config config;
  config.canvas.map_area = CanvasRect{0, 0, 100, 100};
  config.rooms.push_back(rect_room("a", 0.0, 0.0, 0.5, 1.0));
  config.rooms.push_back(rect_room("b", 0.5, 0.0, 1.0, 1.0));
  config.heat.grid_cols = 10;
  config.heat.grid_rows = 10;
  config.heat.idw_power = 2.0;
  return config;
}

// A deliberately naive reference: every cell scans every sample, filters by room, and weights
// with std::pow on the true distance. Shares no code with the implementation beyond the mask.
HeatField reference_field(const Config& config, const RoomMask& mask,
                          const std::vector<HeatSample>& samples) {
  HeatField field{mask.cols, mask.rows, std::vector<HeatCell>(mask.room_index.size(), HeatCell{})};
  const double w = config.canvas.map_area.width;
  const double h = config.canvas.map_area.height;
  for (std::int32_t r = 0; r < mask.rows; ++r) {
    for (std::int32_t c = 0; c < mask.cols; ++c) {
      const std::int16_t room = mask.room_index[mask.index(c, r)];
      HeatCell& out = field.cells[mask.index(c, r)];
      if (room == RoomMask::kOutside) continue;
      if (config.rooms[static_cast<std::size_t>(room)].outdoor) continue;
      const double cx = (c + 0.5) / mask.cols * w;
      const double cy = (r + 0.5) / mask.rows * h;
      double num = 0.0;
      double den = 0.0;
      bool any = false;
      bool exact = false;
      for (const HeatSample& s : samples) {
        if (s.room_index != room || !std::isfinite(s.value) || !std::isfinite(s.position.x) ||
            !std::isfinite(s.position.y))
          continue;
        any = true;
        const double d = std::hypot(cx - static_cast<double>(s.position.x) * w,
                                    cy - static_cast<double>(s.position.y) * h);
        if (d < 0.5) {
          out.value = s.value;
          exact = true;
          break;
        }
        const double wt = 1.0 / std::pow(d, config.heat.idw_power);
        num += wt * static_cast<double>(s.value);
        den += wt;
      }
      if (!any) {
        out.state = CellState::NoData;
        continue;
      }
      out.state = CellState::Value;
      if (!exact) out.value = static_cast<float>(num / den);
    }
  }
  return field;
}

void check_matches_reference(const HeatField& got, const HeatField& want) {
  REQUIRE(got.cols == want.cols);
  REQUIRE(got.rows == want.rows);
  REQUIRE(got.cells.size() == want.cells.size());
  int mismatches = 0;
  for (std::size_t i = 0; i < got.cells.size(); ++i) {
    if (got.cells[i].state != want.cells[i].state) {
      ++mismatches;
      continue;
    }
    if (got.cells[i].state != CellState::Value) continue;
    const double tol = 1e-4 * std::max(1.0, std::fabs(static_cast<double>(want.cells[i].value)));
    if (std::fabs(static_cast<double>(got.cells[i].value - want.cells[i].value)) > tol) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
}

// Bit-for-bit equality of every cell's state and float value. Not memcmp over HeatCell: the
// struct has padding after `state`, whose bytes are indeterminate and carry no meaning.
bool same_bits(const HeatField& a, const HeatField& b) {
  if (a.cols != b.cols || a.rows != b.rows || a.cells.size() != b.cells.size()) return false;
  for (std::size_t i = 0; i < a.cells.size(); ++i) {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::memcpy(&x, &a.cells[i].value, sizeof x);
    std::memcpy(&y, &b.cells[i].value, sizeof y);
    if (a.cells[i].state != b.cells[i].state || x != y) return false;
  }
  return true;
}

std::uint64_t fnv1a(const HeatField& field) {
  std::uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](std::uint8_t byte) {
    h ^= byte;
    h *= 1099511628211ULL;
  };
  for (const HeatCell& cell : field.cells) {
    mix(static_cast<std::uint8_t>(cell.state));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &cell.value, sizeof bits);
    for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(bits >> (8 * i)));
  }
  return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// build_room_mask
// ---------------------------------------------------------------------------------------------

TEST_CASE("build_room_mask: geometry and the example flat's rooms") {
  const Config config = fixtures::example_flat_config();
  const RoomMask mask = build_room_mask(config);
  REQUIRE(mask.cols == 48);
  REQUIRE(mask.rows == 40);
  REQUIRE(mask.room_index.size() == 48U * 40U);
  CHECK(room_at(mask, 0.2, 0.3) == 0);  // living
  CHECK(room_at(mask, 0.5, 0.2) == 1);  // kitchen
  CHECK(room_at(mask, 0.8, 0.2) == 2);  // study
  CHECK(room_at(mask, 0.8, 0.5) == 3);  // hallway, the bar of its L
  CHECK(room_at(mask, 0.5, 0.9) == 3);  // hallway, the leg of its L
  CHECK(room_at(mask, 0.7, 0.8) == 4);  // bathroom, in the hallway's notch (concave room)
  CHECK(room_at(mask, 0.9, 0.8) == 5);  // bedroom
  CHECK(room_at(mask, 0.2, 0.8) == 6);  // balcony
  // The flat tiles the whole map, so every cell centre belongs to some room.
  for (const std::int16_t index : mask.room_index) CHECK(index != RoomMask::kOutside);
}

TEST_CASE("build_room_mask: cells outside every room are kOutside") {
  Config config = two_rooms();
  config.rooms[1].polygon = {{0.5, 0.0}, {1.0, 0.0}, {1.0, 0.5}, {0.5, 0.5}};  // B: top half only
  const RoomMask mask = build_room_mask(config);
  CHECK(room_at(mask, 0.75, 0.25) == 1);
  CHECK(room_at(mask, 0.75, 0.75) == RoomMask::kOutside);
}

TEST_CASE("build_room_mask: where rooms overlap, the first room in config order wins") {
  Config config = two_rooms();
  config.rooms.push_back(rect_room("over", 0.0, 0.0, 1.0, 1.0));  // covers both, listed last
  config.rooms.insert(config.rooms.begin(), rect_room("first", 0.4, 0.4, 0.6, 0.6));
  const RoomMask mask = build_room_mask(config);
  CHECK(room_at(mask, 0.45, 0.45) == 0);  // "first", although "a" and "over" also contain it
  CHECK(room_at(mask, 0.25, 0.25) == 1);  // "a"
  CHECK(room_at(mask, 0.75, 0.75) == 2);  // "b", never "over"
}

TEST_CASE("build_room_mask: a degenerate polygon claims no cells") {
  Config config = two_rooms();
  Room two_points;
  two_points.id = "line";
  two_points.polygon = {{0.0, 0.0}, {1.0, 1.0}};
  Room zero_area;
  zero_area.id = "flat";
  zero_area.polygon = {{0.0, 0.0}, {0.5, 0.5}, {1.0, 1.0}};
  config.rooms.insert(config.rooms.begin(), {two_points, zero_area});
  const RoomMask mask = build_room_mask(config);
  for (const std::int16_t index : mask.room_index) {
    CHECK(index != 0);
    CHECK(index != 1);
  }
  CHECK(room_at(mask, 0.25, 0.5) == 2);  // "a" still claims its cells
}

TEST_CASE("build_room_mask: a 1 x 1 grid samples the map's centre") {
  Config config = two_rooms();
  config.heat.grid_cols = 1;
  config.heat.grid_rows = 1;
  const RoomMask mask = build_room_mask(config);
  REQUIRE(mask.room_index.size() == 1U);
  // Centre (0.5, 0.5) sits on the shared wall, which belongs to the room on its right.
  CHECK(mask.room_index[0] == 1);
}

TEST_CASE("build_room_mask: a non-positive grid gives an empty mask") {
  Config config = two_rooms();
  SUBCASE("zero columns") {
    config.heat.grid_cols = 0;
  }
  SUBCASE("negative rows") {
    config.heat.grid_rows = -3;
  }
  const RoomMask mask = build_room_mask(config);
  CHECK(mask.cols == 0);
  CHECK(mask.rows == 0);
  CHECK(mask.room_index.empty());
}

TEST_CASE("build_room_mask: rooms past the int16 range are never assigned") {
  Config config;
  config.heat.grid_cols = 2;
  config.heat.grid_rows = 2;
  // 32766 empty rooms, then a room covering the map at index 32766 (the last nameable index).
  config.rooms.resize(32766);
  config.rooms.push_back(rect_room("last_nameable", 0.0, 0.0, 1.0, 1.0));
  SUBCASE("index 32766 is assigned (positive control)") {
    const RoomMask mask = build_room_mask(config);
    for (const std::int16_t index : mask.room_index) CHECK(index == 32766);
  }
  SUBCASE("index 32767 would collide with the int16 limit: never assigned") {
    config.rooms.back().polygon.clear();
    config.rooms.push_back(rect_room("too_far", 0.0, 0.0, 1.0, 1.0));
    const RoomMask mask = build_room_mask(config);
    for (const std::int16_t index : mask.room_index) CHECK(index == RoomMask::kOutside);
  }
}

// ---------------------------------------------------------------------------------------------
// compute_heat_field
// ---------------------------------------------------------------------------------------------

TEST_CASE("compute_heat_field: a single sample gives its room a flat field") {
  const Config config = two_rooms();
  const RoomMask mask = build_room_mask(config);
  const HeatField field = compute_heat_field(config, mask, {{{0.2F, 0.3F}, 0, 21.5}});
  REQUIRE(field.cols == 10);
  REQUIRE(field.rows == 10);
  for (std::int32_t r = 0; r < 10; ++r) {
    for (std::int32_t c = 0; c < 10; ++c) {
      const HeatCell& cell = cell_at(field, c, r);
      if (c < 5) {
        CHECK(cell.state == CellState::Value);
        CHECK(cell.value == 21.5F);
      } else {
        CHECK(cell.state == CellState::NoData);
      }
    }
  }
}

TEST_CASE("compute_heat_field: a room with no sample is NoData, never a neighbour's value") {
  Config config = two_rooms();
  config.rooms.push_back(rect_room("empty", 0.0, 0.0, 0.0, 0.0));  // claims nothing either
  const RoomMask mask = build_room_mask(config);
  const HeatField field = compute_heat_field(config, mask, {{{0.75F, 0.5F}, 1, 30.0}});
  for (std::int32_t r = 0; r < 10; ++r) {
    for (std::int32_t c = 0; c < 5; ++c) {
      CHECK(cell_at(field, c, r).state == CellState::NoData);
      CHECK(cell_at(field, c, r).value == 0.0F);
    }
  }
}

TEST_CASE("compute_heat_field: cells outside every room stay Outside") {
  Config config = two_rooms();
  config.rooms[1].polygon = {{0.5, 0.0}, {1.0, 0.0}, {1.0, 0.5}, {0.5, 0.5}};
  const RoomMask mask = build_room_mask(config);
  const HeatField field =
      compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 20.0}, {{0.75F, 0.25F}, 1, 25.0}});
  CHECK(cell_at(field, 7, 7).state == CellState::Outside);
  CHECK(cell_at(field, 7, 2).state == CellState::Value);
  CHECK(cell_at(field, 2, 7).state == CellState::Value);
}

TEST_CASE("compute_heat_field: interpolation never crosses a room boundary") {
  // Room B holds an extreme sample hard against the shared wall; room A's samples are 10 and
  // 20. If any of B's influence leaked through the wall, A's cells next to it would leave the
  // 10..20 range. Verified red against a planted violation (all samples feeding all rooms).
  const Config config = two_rooms();
  const RoomMask mask = build_room_mask(config);
  const std::vector<HeatSample> samples{
      {{0.1F, 0.1F}, 0, 10.0}, {{0.1F, 0.9F}, 0, 20.0}, {{0.5F, 0.5F}, 1, 1000.0}};
  const HeatField field = compute_heat_field(config, mask, samples);
  for (std::int32_t r = 0; r < 10; ++r) {
    for (std::int32_t c = 0; c < 5; ++c) {
      const HeatCell& cell = cell_at(field, c, r);
      REQUIRE(cell.state == CellState::Value);
      CHECK(cell.value >= 10.0F);
      CHECK(cell.value <= 20.0F);
    }
    for (std::int32_t c = 5; c < 10; ++c) CHECK(cell_at(field, c, r).value == 1000.0F);
  }
}

TEST_CASE("compute_heat_field: matches a naive per-room reference on a 20-room house") {
  const auto house = fixtures::twenty_rooms_sixty_sensors(60, 48);
  const RoomMask mask = build_room_mask(house.config);
  check_matches_reference(compute_heat_field(house.config, mask, house.samples),
                          reference_field(house.config, mask, house.samples));
}

TEST_CASE("compute_heat_field: matches the reference on the example flat at several powers") {
  Config config = fixtures::example_flat_config();
  const auto samples = fixtures::example_flat_temperatures();
  // Add a second living-room sensor so there is a gradient for the power to shape.
  auto two = samples;
  two.push_back({{0.05F, 0.55F}, 0, 17.0});
  for (const double power : {1.0, 2.0, 3.0, 2.5, 8.0, 0.5}) {
    CAPTURE(power);
    config.heat.idw_power = power;
    const RoomMask mask = build_room_mask(config);
    check_matches_reference(compute_heat_field(config, mask, two),
                            reference_field(config, mask, two));
  }
}

TEST_CASE("compute_heat_field: weights use canvas pixels, not map units") {
  // A 200 x 100 px map: 0.1 map units is 20 px across but only 10 px down. One 1x1 cell at the
  // centre, pixel (100, 50); sample X is 20 px left of it, sample Y 10 px above it.
  Config config;
  config.canvas.map_area = CanvasRect{0, 0, 200, 100};
  config.rooms.push_back(rect_room("only", 0.0, 0.0, 1.0, 1.0));
  config.heat.grid_cols = 1;
  config.heat.grid_rows = 1;
  config.heat.idw_power = 2.0;
  const RoomMask mask = build_room_mask(config);
  const HeatField field =
      compute_heat_field(config, mask, {{{0.4F, 0.5F}, 0, 0.0}, {{0.5F, 0.4F}, 0, 100.0}});
  // w_x = 1/400, w_y = 1/100: value = 100 * (1/100) / (1/400 + 1/100) = 80. Map units would
  // weight them equally and give 50.
  REQUIRE(field.cells.size() == 1U);
  CHECK(field.cells[0].state == CellState::Value);
  CHECK(static_cast<double>(field.cells[0].value) == doctest::Approx(80.0F).epsilon(1e-6));
}

TEST_CASE("compute_heat_field: a 1 x 1 grid") {
  Config config = two_rooms();
  config.heat.grid_cols = 1;
  config.heat.grid_rows = 1;
  const RoomMask mask = build_room_mask(config);
  // The single cell belongs to room B (shared-wall rule); only B's samples count.
  const HeatField field =
      compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 5.0}, {{0.75F, 0.5F}, 1, 7.0}});
  REQUIRE(field.cells.size() == 1U);
  CHECK(field.cells[0].state == CellState::Value);
  CHECK(field.cells[0].value == 7.0F);
}

TEST_CASE("compute_heat_field: a sample within half a pixel of a cell centre sets it exactly") {
  // A 2 x 2 px map with one cell: its centre is pixel (1, 1).
  Config config;
  config.canvas.map_area = CanvasRect{0, 0, 2, 2};
  config.rooms.push_back(rect_room("only", 0.0, 0.0, 1.0, 1.0));
  config.heat.grid_cols = 1;
  config.heat.grid_rows = 1;
  const RoomMask mask = build_room_mask(config);
  const HeatSample far{{0.0F, 0.0F}, 0, 100.0};
  SUBCASE("d = 0.4 px: coincident, the other sample has no say") {
    const HeatField field = compute_heat_field(config, mask, {far, {{0.7F, 0.5F}, 0, 3.0}});
    CHECK(field.cells[0].value == 3.0F);
  }
  SUBCASE("d = 0.5 px exactly: not coincident (strictly less than), so it blends") {
    const HeatField field = compute_heat_field(config, mask, {far, {{0.75F, 0.5F}, 0, 3.0}});
    // d^2 = 0.25 and 2 against the far sample: (3/0.25 + 100/2) / (1/0.25 + 1/2) = 62/4.5.
    CHECK(static_cast<double>(field.cells[0].value) == doctest::Approx(62.0 / 4.5).epsilon(1e-6));
  }
}

TEST_CASE("compute_heat_field: coincident sensors") {
  // Two sensors at the same spot, exactly on the centre of cell (2, 2) of room A.
  const Config config = two_rooms();
  const RoomMask mask = build_room_mask(config);
  const std::vector<HeatSample> samples{{{0.25F, 0.25F}, 0, 10.0}, {{0.25F, 0.25F}, 0, 20.0}};
  const HeatField field = compute_heat_field(config, mask, samples);
  // On the spot: the first in input order wins outright, no average.
  CHECK(cell_at(field, 2, 2).value == 10.0F);
  // Everywhere else in the room they weigh the same: the mean, with no division by zero.
  CHECK(static_cast<double>(cell_at(field, 0, 9).value) == doctest::Approx(15.0F));
  CHECK(static_cast<double>(cell_at(field, 4, 0).value) == doctest::Approx(15.0F));
  // Swapping the order swaps the winner on the spot.
  const HeatField swapped = compute_heat_field(config, mask, {samples[1], samples[0]});
  CHECK(cell_at(swapped, 2, 2).value == 20.0F);
}

TEST_CASE("compute_heat_field: a sensor exactly on a wall feeds only the room it names") {
  // Both sensors sit on the shared wall x = 0.5; each is credited to the room its pin names.
  const Config config = two_rooms();
  const RoomMask mask = build_room_mask(config);
  SUBCASE("named for A: A gets its value, B has no data") {
    const HeatField field = compute_heat_field(config, mask, {{{0.5F, 0.5F}, 0, 12.0}});
    CHECK(cell_at(field, 4, 5).value == 12.0F);
    CHECK(cell_at(field, 5, 5).state == CellState::NoData);
  }
  SUBCASE("named for B: the reverse") {
    const HeatField field = compute_heat_field(config, mask, {{{0.5F, 0.5F}, 1, 12.0}});
    CHECK(cell_at(field, 4, 5).state == CellState::NoData);
    CHECK(cell_at(field, 5, 5).value == 12.0F);
  }
}

TEST_CASE("compute_heat_field: sensors outside every room") {
  Config config = two_rooms();
  config.rooms[1].polygon = {{0.5, 0.0}, {1.0, 0.0}, {1.0, 0.5}, {0.5, 0.5}};  // B: top half
  const RoomMask mask = build_room_mask(config);
  SUBCASE("position outside every polygon, but named for B: it still feeds B") {
    // room_index is the pin's declared room, which is authoritative (validate() warns about the
    // position, it does not reassign the pin).
    const HeatField field = compute_heat_field(config, mask, {{{0.75F, 0.9F}, 1, 8.0}});
    CHECK(cell_at(field, 7, 2).value == 8.0F);
    CHECK(cell_at(field, 7, 7).state == CellState::Outside);
    CHECK(cell_at(field, 2, 2).state == CellState::NoData);
  }
  SUBCASE("room_index kOutside: ignored everywhere") {
    const HeatField field =
        compute_heat_field(config, mask, {{{0.75F, 0.9F}, RoomMask::kOutside, 8.0}});
    CHECK(cell_at(field, 7, 2).state == CellState::NoData);
    CHECK(cell_at(field, 2, 2).state == CellState::NoData);
  }
}

TEST_CASE("compute_heat_field: unusable samples are ignored, leaving NoData") {
  const Config config = two_rooms();
  const RoomMask mask = build_room_mask(config);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  SUBCASE("room index past the end of config.rooms") {
    const HeatField field = compute_heat_field(config, mask, {{{0.25F, 0.5F}, 2, 5.0}});
    CHECK(cell_at(field, 2, 5).state == CellState::NoData);
  }
  SUBCASE("negative room index other than kOutside") {
    const HeatField field = compute_heat_field(config, mask, {{{0.25F, 0.5F}, -7, 5.0}});
    CHECK(cell_at(field, 2, 5).state == CellState::NoData);
  }
  SUBCASE("NaN value") {
    const HeatField field = compute_heat_field(
        config, mask, {{{0.25F, 0.5F}, 0, std::numeric_limits<float>::quiet_NaN()}});
    CHECK(cell_at(field, 2, 5).state == CellState::NoData);
  }
  SUBCASE("infinite position") {
    const HeatField field = compute_heat_field(config, mask, {{{inf, 0.5F}, 0, 5.0F}});
    CHECK(cell_at(field, 2, 5).state == CellState::NoData);
  }
  SUBCASE("NaN y position, next to a good sample: only the good one counts") {
    const HeatField field =
        compute_heat_field(config, mask, {{{0.25F, nan}, 0, 99.0F}, {{0.25F, 0.5F}, 0, 5.0}});
    CHECK(cell_at(field, 2, 5).value == 5.0F);
    CHECK(cell_at(field, 0, 0).value == 5.0F);
  }
}

TEST_CASE("compute_heat_field: a mask that is not a mask gives an empty field") {
  const Config config = two_rooms();
  RoomMask mask = build_room_mask(config);
  SUBCASE("wrong cell count") {
    mask.room_index.pop_back();
  }
  SUBCASE("zero columns") {
    mask.cols = 0;
  }
  SUBCASE("negative rows") {
    mask.rows = -1;
  }
  const HeatField field = compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 5.0}});
  CHECK(field.cols == 0);
  CHECK(field.rows == 0);
  CHECK(field.cells.empty());
}

TEST_CASE("compute_heat_field: a mask cell naming a room the config lacks is Outside") {
  const Config config = two_rooms();
  RoomMask mask = build_room_mask(config);
  mask.room_index[mask.index(7, 7)] = 5;  // from some other, larger config
  const HeatField field =
      compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 5.0}, {{0.75F, 0.5F}, 1, 6.0}});
  CHECK(cell_at(field, 7, 7).state == CellState::Outside);
  CHECK(cell_at(field, 6, 7).value == 6.0F);
}

TEST_CASE("compute_heat_field: no samples at all") {
  const Config config = two_rooms();
  const HeatField field = compute_heat_field(config, build_room_mask(config), {});
  for (const HeatCell& cell : field.cells) CHECK(cell.state == CellState::NoData);
}

TEST_CASE("compute_heat_field: same inputs give bit-identical fields") {
  const auto house = fixtures::twenty_rooms_sixty_sensors();
  const RoomMask mask = build_room_mask(house.config);
  const HeatField first = compute_heat_field(house.config, mask, house.samples);
  const HeatField second =
      compute_heat_field(house.config, build_room_mask(house.config), house.samples);
  CHECK(same_bits(first, second));
}

TEST_CASE("compute_heat_field: outdoor rooms get no heat colour (D-006)") {
  Config config = two_rooms();
  config.rooms[1].outdoor = true;
  const RoomMask mask = build_room_mask(config);
  // The mask still knows the outdoor room (hit-testing needs it) ...
  CHECK(mask.room_index[mask.index(7, 5)] == 1);
  // ... but the field leaves it uncoloured, sample or not, and A is unaffected.
  const HeatField field =
      compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 20.0F}, {{0.75F, 0.5F}, 1, 5.0F}});
  for (std::int32_t r = 0; r < 10; ++r) {
    for (std::int32_t c = 0; c < 5; ++c) CHECK(cell_at(field, c, r).value == 20.0F);
    for (std::int32_t c = 5; c < 10; ++c) CHECK(cell_at(field, c, r).state == CellState::Outside);
  }
}

TEST_CASE("compute_heat_field: the example flat's balcony is Outside, the flat is coloured") {
  const Config config = fixtures::example_flat_config();
  const RoomMask mask = build_room_mask(config);
  const HeatField field = compute_heat_field(config, mask, fixtures::example_flat_temperatures());
  for (std::size_t i = 0; i < field.cells.size(); ++i) {
    if (mask.room_index[i] == 6) {
      CHECK(field.cells[i].state == CellState::Outside);
    } else {
      CHECK(field.cells[i].state == CellState::Value);
    }
  }
}

TEST_CASE("reuse forms allocate nothing once warm (D-022)") {
  const auto house = fixtures::twenty_rooms_sixty_sensors();
  RoomMask mask;
  HeatField field;
  build_room_mask(house.config, mask);  // warm-up: these may allocate
  compute_heat_field(house.config, mask, house.samples, field);
  const std::size_t before = g_allocations;
  for (int i = 0; i < 3; ++i) {
    build_room_mask(house.config, mask);
    compute_heat_field(house.config, mask, house.samples, field);
  }
  const std::size_t during = g_allocations - before;
  CHECK(during == 0);
  // Positive control: the counter does see the returning form allocate.
  const std::size_t control = g_allocations;
  const HeatField fresh = compute_heat_field(house.config, mask, house.samples);
  CHECK(g_allocations > control);
  CHECK(fresh.cells.size() == field.cells.size());
}

TEST_CASE("reuse forms give the same result as the returning forms") {
  const auto house = fixtures::twenty_rooms_sixty_sensors(40, 30);
  RoomMask mask;
  mask.room_index.assign(5000, 3);  // stale contents from a bigger, different mask
  build_room_mask(house.config, mask);
  const RoomMask fresh_mask = build_room_mask(house.config);
  CHECK(mask.cols == fresh_mask.cols);
  CHECK(mask.rows == fresh_mask.rows);
  CHECK(mask.room_index == fresh_mask.room_index);
  HeatField field;
  field.cells.assign(9000, HeatCell{CellState::Value, 1.0F});  // stale contents
  compute_heat_field(house.config, mask, house.samples, field);
  const HeatField fresh = compute_heat_field(house.config, mask, house.samples);
  CHECK(same_bits(field, fresh));
}

TEST_CASE("reuse forms clear stale output on refusal") {
  Config config = two_rooms();
  RoomMask mask = build_room_mask(config);
  HeatField field = compute_heat_field(config, mask, {{{0.25F, 0.5F}, 0, 5.0F}});
  REQUIRE_FALSE(field.cells.empty());
  RoomMask broken = mask;
  broken.room_index.pop_back();
  compute_heat_field(config, broken, {{{0.25F, 0.5F}, 0, 5.0F}}, field);
  CHECK(field.cols == 0);
  CHECK(field.cells.empty());
  config.heat.grid_cols = 0;
  build_room_mask(config, mask);
  CHECK(mask.cols == 0);
  CHECK(mask.rows == 0);
  CHECK(mask.room_index.empty());
}

TEST_CASE("compute_heat_field: the example flat's field is the same on every platform") {
  // Golden hash over every cell's state and float bits. Integer powers use no libm call beyond
  // sqrtf, and core is built with -ffp-contract=off (D-024), so GCC and Clang on any IEEE-754
  // platform must agree. A change here means golden images change too: say why in the PR.
  Config config = fixtures::example_flat_config();
  auto samples = fixtures::example_flat_temperatures();
  samples.push_back({{0.05F, 0.55F}, 0, 17.0F});  // a gradient in the living room
  const RoomMask mask = build_room_mask(config);
  CHECK(fnv1a(compute_heat_field(config, mask, samples)) == 0x2c0702a208c05dfdULL);
  config.heat.idw_power = 3.0;
  CHECK(fnv1a(compute_heat_field(config, mask, samples)) == 0x098e16e1daa4e02fULL);
}
