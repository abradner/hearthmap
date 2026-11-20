// The whole core pipeline on the example flat, end to end:
//
//   config + readings -> validate, build_view_model, heat_samples -> compute_heat_field
//   -> build_colour_lut(active layer's band) -> one colour per cell
//
// Each stage has its own tests; this one checks that the stages agree with each other at the
// seams: heat_samples() hands compute_heat_field() float samples in the right rooms, the
// outdoor rule (D-006) is applied once, in the heat field, and the active layer's band is the
// one that colours the field.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/readings.h"
#include "hearthmap/view_model.h"

using namespace hearthmap;
using namespace hearthmap::test;

namespace {

struct Frame {
  ViewModel vm;
  std::vector<HeatSample> samples;
  RoomMask mask;
  HeatField field;
  ColourLut lut;
};

// What the device does per refresh, for the layer the UI asked for.
Frame render(const Config& config, const Readings& readings, Layer requested) {
  Frame f;
  UiState ui = initial_ui_state(config);
  ui.active_layer = requested;
  build_view_model(config, readings, kFlatNow, ui, f.vm);
  heat_samples(config, readings, kFlatNow, f.vm.active_layer, f.samples);
  build_room_mask(config, f.mask);
  compute_heat_field(config, f.mask, f.samples, f.field);
  build_colour_lut(config.layer(f.vm.active_layer).band, Palette{}, kDefaultColourLutSize, f.lut);
  return f;
}

// Every cell of room `room`, with its colour. Fails the test if the room has no cells.
struct RoomCells {
  std::vector<HeatCell> cells;
  std::vector<Rgb565> colours;
};

RoomCells cells_of(const Frame& f, std::size_t room) {
  RoomCells out;
  REQUIRE(f.field.cols == f.mask.cols);
  REQUIRE(f.field.rows == f.mask.rows);
  for (std::size_t i = 0; i < f.field.cells.size(); ++i) {
    if (f.mask.room_index[i] != static_cast<std::int16_t>(room)) continue;
    out.cells.push_back(f.field.cells[i]);
    out.colours.push_back(f.lut.lookup(f.field.cells[i].value));
  }
  REQUIRE_FALSE(out.cells.empty());
  return out;
}

bool all_in_state(const RoomCells& rc, CellState s) {
  for (const auto& c : rc.cells) {
    if (c.state != s) return false;
  }
  return true;
}

Severity pulse_of(const ViewModel& vm, std::size_t room) {
  for (const auto& r : vm.rooms) {
    if (r.room_index == room) return r.pulse;
  }
  FAIL("room missing from the view model");
  return Severity::Stale;
}

}  // namespace

TEST_CASE("pipeline: the example flat validates with no errors") {
  for (const auto& issue : validate(flat_config())) {
    INFO(issue.path << ": " << issue.message);
    CHECK(issue.severity != IssueSeverity::Error);
  }
}

TEST_CASE("pipeline: humid bathroom, humidity layer") {
  const Config config = flat_config();
  const Frame f = render(config, flat_humid_bathroom(), Layer::Humidity);
  REQUIRE(f.vm.active_layer == Layer::Humidity);

  const std::size_t bathroom = room_index(config, "bathroom");
  const std::size_t bedroom = room_index(config, "bedroom");
  const std::size_t living = room_index(config, "living");
  const std::size_t kitchen = room_index(config, "kitchen");
  const std::size_t hallway = room_index(config, "hallway");
  const std::size_t balcony = room_index(config, "balcony");
  const Rgb565 neutral = to_rgb565(Palette{}.neutral);

  SUBCASE("rooms with no humidity sensor are NoData, not borrowed from a neighbour") {
    CHECK(all_in_state(cells_of(f, kitchen), CellState::NoData));
    CHECK(all_in_state(cells_of(f, hallway), CellState::NoData));
  }

  SUBCASE("the balcony's sample is handed over, and the heat field alone makes it Outside") {
    bool balcony_sample = false;
    for (const auto& s : f.samples) {
      if (s.room_index == static_cast<std::int16_t>(balcony)) {
        balcony_sample = true;
        CHECK(s.value == 92.0F);
      }
    }
    CHECK(balcony_sample);
    CHECK(all_in_state(cells_of(f, balcony), CellState::Outside));
    CHECK(pulse_of(f.vm, balcony) == Severity::Ok);
  }

  SUBCASE("the bathroom's cells are its one reading, coloured on the hot side") {
    const RoomCells rc = cells_of(f, bathroom);
    CHECK(all_in_state(rc, CellState::Value));
    for (std::size_t i = 0; i < rc.cells.size(); ++i) {
      CHECK(rc.cells[i].value == 82.0F);
      const Rgb888 c = from_rgb565(rc.colours[i]);
      CHECK(rc.colours[i] != neutral);
      CHECK(c.r > c.b);  // towards the hot colour, not the cold one
      CHECK(rc.colours[i] == colour565_for(config.layer(Layer::Humidity).band, 82.0));
    }
  }

  SUBCASE("living room humidity comes from its climate pin only (plant humidity is display-only)") {
    const RoomCells rc = cells_of(f, living);
    CHECK(all_in_state(rc, CellState::Value));
    for (std::size_t i = 0; i < rc.cells.size(); ++i) {
      CHECK(rc.cells[i].value == 48.0F);
      CHECK(rc.colours[i] == neutral);  // 48 is inside 40..60
    }
  }

  SUBCASE("the bathroom pulses at danger, the bedroom at warn, nothing else") {
    CHECK(pulse_of(f.vm, bathroom) == Severity::Danger);
    CHECK(pulse_of(f.vm, bedroom) == Severity::Warn);
    for (const auto& r : f.vm.rooms) {
      if (r.room_index == bathroom || r.room_index == bedroom) continue;
      INFO("room " << config.rooms[r.room_index].id);
      CHECK(r.pulse == Severity::Ok);
    }
  }
}

TEST_CASE("pipeline: humid bathroom, temperature layer pulses nothing") {
  const Config config = flat_config();
  const Frame f = render(config, flat_humid_bathroom(), Layer::Temperature);
  REQUIRE(f.vm.active_layer == Layer::Temperature);
  for (const auto& r : f.vm.rooms) {
    INFO("room " << config.rooms[r.room_index].id);
    CHECK(r.pulse == Severity::Ok);
  }
  // Every indoor room has a temperature sensor, so every indoor cell has a value; the balcony
  // (9 degrees, far below the band) is still Outside rather than full cold.
  for (std::size_t i = 0; i < f.field.cells.size(); ++i) {
    const std::int16_t room = f.mask.room_index[i];
    if (room == RoomMask::kOutside) continue;
    const bool outdoor = config.rooms[static_cast<std::size_t>(room)].outdoor;
    CHECK(f.field.cells[i].state == (outdoor ? CellState::Outside : CellState::Value));
  }
}
