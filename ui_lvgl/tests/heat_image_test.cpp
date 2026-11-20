#include <doctest/doctest.h>

#include <vector>

#include "fixtures.h"
#include "hearthmap/ui/heat_image.h"
#include "hearthmap/ui/theme.h"

using namespace hearthmap;
using namespace hearthmap::ui;

namespace {

constexpr Rgb565 c565(std::uint32_t rgb) {
  return to_rgb565(Rgb888{static_cast<std::uint8_t>(rgb >> 16U),
                          static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU),
                          static_cast<std::uint8_t>(rgb & 0xFFU)});
}

// A field where every cell of room `r` (by the mask the rasterizer itself uses) is Value with
// colour `colours[r]`, or NoData when colours[r] == 0.
struct Built {
  HeatField field;
  std::vector<Rgb565> colours;
};

Built uniform(const Config& c, const std::vector<Rgb565>& per_room) {
  const RoomMask mask = build_room_mask(c);
  Built b;
  b.field.cols = mask.cols;
  b.field.rows = mask.rows;
  b.field.cells.resize(mask.room_index.size());
  b.colours.resize(mask.room_index.size(), 0);
  for (std::size_t i = 0; i < mask.room_index.size(); ++i) {
    const auto r = mask.room_index[i];
    if (r < 0 || c.rooms[static_cast<std::size_t>(r)].outdoor) {
      b.field.cells[i].state = CellState::Outside;
    } else if (per_room[static_cast<std::size_t>(r)] == 0) {
      b.field.cells[i].state = CellState::NoData;
    } else {
      b.field.cells[i].state = CellState::Value;
      b.colours[i] = per_room[static_cast<std::size_t>(r)];
    }
  }
  return b;
}

Rgb565 at(const std::vector<Rgb565>& img, const HeatRasterizer& r, int x, int y) {
  return img[static_cast<std::size_t>(y) * static_cast<std::size_t>(r.width()) +
             static_cast<std::size_t>(x)];
}

}  // namespace

TEST_CASE("heat image covers the map area exactly") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  CHECK(r.width() == 864);
  CHECK(r.height() == 720);
  CHECK(r.pixel_count() == 864U * 720U);
  CHECK(r.working_bytes() >= r.pixel_count());
}

TEST_CASE("each pixel takes its own room's colour, right up to the wall") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  const Rgb565 red = c565(0xE34948);
  const Rgb565 blue = c565(0x2A78D6);
  Built b = uniform(c, {red, blue, 0});
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(b.field, b.colours, img.data());
  // The wall between west and east is at x = 432. No blending across it, even one pixel away.
  CHECK(at(img, r, 431, 100) == red);
  CHECK(at(img, r, 432, 100) == blue);
  CHECK(at(img, r, 0, 0) == red);       // corner
  CHECK(at(img, r, 863, 359) == blue);  // corner next to the balcony
  // Every pixel of the west room is exactly red.
  bool all_red = true;
  for (int y = 0; y < 360; ++y) {
    for (int x = 0; x < 432; ++x) all_red = all_red && at(img, r, x, y) == red;
  }
  CHECK(all_red);
}

TEST_CASE("outdoor rooms get the outdoor fill, never heat colour") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  Built b = uniform(c, {c565(0xE34948), c565(0x2A78D6), 0});
  // Even if a field claimed values in the balcony, it is drawn as outdoor.
  const RoomMask mask = build_room_mask(c);
  for (std::size_t i = 0; i < mask.room_index.size(); ++i) {
    if (mask.room_index[i] == 2) {
      b.field.cells[i].state = CellState::Value;
      b.colours[i] = c565(0xE34948);
    }
  }
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(b.field, b.colours, img.data());
  CHECK(at(img, r, 400, 500) == c565(theme::kOutdoorFill));
  CHECK(at(img, r, 0, 719) == c565(theme::kOutdoorFill));
}

TEST_CASE("an unmeasured room is drawn as no-data stripes") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  Built b = uniform(c, {c565(0xE34948), 0, 0});
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(b.field, b.colours, img.data());
  const Rgb565 a = c565(theme::kNoDataA);
  const Rgb565 bb = c565(theme::kNoDataB);
  CHECK(at(img, r, 440, 0) == bb);  // (440 + 0) / 8 = 55, odd: B
  CHECK(at(img, r, 440, 8) == a);   // 56, even: A
  CHECK(at(img, r, 600, 200) != c565(0xE34948));
}

TEST_CASE("pixels outside every room are background") {
  Config c = test::small_config();
  c.rooms.pop_back();  // drop the balcony: the bottom half belongs to no room
  for (auto& p : c.pins) p.room_id = p.room_id == "balcony" ? "west" : p.room_id;
  HeatRasterizer r(c);
  Built b = uniform(c, {c565(0xE34948), c565(0x2A78D6)});
  std::vector<Rgb565> img(r.pixel_count(), 1);
  r.render(b.field, b.colours, img.data());
  CHECK(at(img, r, 100, 600) == c565(theme::kBackground));
}

TEST_CASE("values blend between cells of the same room") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  Built b = uniform(c, {c565(0x080808), c565(0x2A78D6), 0});  // 0 would mean NoData
  // West room: left half of its cells black, right half white -> a ramp in between.
  for (std::int32_t row = 0; row < b.field.rows; ++row) {
    for (std::int32_t col = 0; col < b.field.cols; ++col) {
      const auto i = static_cast<std::size_t>(row * b.field.cols + col);
      if (b.field.cells[i].state == CellState::Value && col >= 12 && col < 24) {
        b.colours[i] = c565(0xFFFFFF);
      }
    }
  }
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(b.field, b.colours, img.data());
  // Cell 11's centre is at x = 11.5 * 18 = 207, cell 12's at 225. Midway is a mid grey.
  const Rgb565 mid = at(img, r, 216, 100);
  const unsigned g = (mid >> 5U) & 0x3FU;
  CHECK(g > 20U);
  CHECK(g < 44U);
  CHECK(at(img, r, 200, 100) != mid);
}

TEST_CASE("a field of the wrong size draws no data rather than reading out of bounds") {
  const Config c = test::small_config();
  HeatRasterizer r(c);
  HeatField wrong;
  wrong.cols = 2;
  wrong.rows = 2;
  wrong.cells.resize(4);
  for (auto& cell : wrong.cells) cell.state = CellState::Value;
  std::vector<Rgb565> colours(4, c565(0xE34948));
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(wrong, colours, img.data());
  const Rgb565 px = at(img, r, 100, 100);
  CHECK((px == c565(theme::kNoDataA) || px == c565(theme::kNoDataB)));
}

TEST_CASE("colour_cells colours Value cells through the lookup table and nothing else") {
  HeatField f;
  f.cols = 3;
  f.rows = 1;
  f.cells = {HeatCell{CellState::Value, 30.0F}, HeatCell{CellState::NoData, 0.0F},
             HeatCell{CellState::Value, 21.0F}};
  const Band band = default_band(Layer::Temperature);
  const ColourLut lut = build_colour_lut(band, theme::kHeatPalette);
  std::vector<Rgb565> out;
  colour_cells(f, lut, out);
  REQUIRE(out.size() == 3);
  CHECK(out[0] == lut.lookup(30.0F));
  CHECK(out[1] == 0);
  CHECK(out[2] == lut.lookup(21.0F));
  CHECK(out[0] != out[2]);
  // Warm: no allocation once sized (same storage).
  const Rgb565* before = out.data();
  colour_cells(f, lut, out);
  CHECK(out.data() == before);
}

TEST_CASE("a measured room's narrow extension is coloured, not striped") {
  // Review finding: pixels more than two cells from any Value cell of their own room drew the
  // no-data stripes although the room has data.
  Config c;
  c.canvas.map_area = CanvasRect{0, 0, 800, 800};
  c.heat.grid_cols = 8;
  c.heat.grid_rows = 8;
  // A wide base (y 0.5..1) with a chimney x 0.45..0.55 up to the top. Cell centres sit at
  // 0.0625 + k * 0.125, so no centre falls inside the chimney.
  c.rooms = {Room{"a",
                  "A",
                  {{0.45, 0.0},
                   {0.55, 0.0},
                   {0.55, 0.5},
                   {1.0, 0.5},
                   {1.0, 1.0},
                   {0.0, 1.0},
                   {0.0, 0.5},
                   {0.45, 0.5}},
                  false}};
  HeatRasterizer r(c);
  const Rgb565 red = c565(0xE34948);
  Built b = uniform(c, {red});
  std::vector<Rgb565> img(r.pixel_count(), 0);
  r.render(b.field, b.colours, img.data());
  CHECK(at(img, r, 400, 10) == red);  // top of the chimney, four cells from the base
  CHECK(at(img, r, 400, 700) == red);
}

TEST_CASE("rows rendered in slices from one prepare() equal render(), fallback cells included") {
  // The device prepares once per image and renders eight rows per step (F5 of the follow-up
  // review): the result must be exactly render()'s, including pixels that need a room's first
  // Value cell (the chimney below), which is what prepare() precomputes.
  Config c;
  c.canvas.map_area = CanvasRect{0, 0, 800, 800};
  c.heat.grid_cols = 8;
  c.heat.grid_rows = 8;
  c.rooms = {Room{"a",
                  "A",
                  {{0.45, 0.0},
                   {0.55, 0.0},
                   {0.55, 0.5},
                   {1.0, 0.5},
                   {1.0, 1.0},
                   {0.0, 1.0},
                   {0.0, 0.5},
                   {0.45, 0.5}},
                  false}};
  HeatRasterizer r(c);
  Built b = uniform(c, {c565(0xE34948)});
  std::vector<Rgb565> whole(r.pixel_count(), 0);
  r.render(b.field, b.colours, whole.data());
  std::vector<Rgb565> sliced(r.pixel_count(), 1);
  const HeatRasterizer::Prepared p = r.prepare(b.field, b.colours);
  CHECK(p.field_ok);
  for (std::int32_t y = 0; y < r.height(); y += 8) {
    r.render_rows(p, b.field, b.colours, sliced.data(), y, y + 8);
  }
  CHECK(sliced == whole);
  // A Prepared for a fitting field does not let a wrong-sized field be read: no data, as render().
  HeatField wrong;
  wrong.cols = 3;
  wrong.rows = 3;
  wrong.cells.resize(9);
  std::vector<Rgb565> guarded(r.pixel_count(), 1);
  r.render_rows(p, wrong, b.colours, guarded.data(), 0, r.height());
  std::vector<Rgb565> expected(r.pixel_count(), 1);
  r.render(wrong, b.colours, expected.data());
  CHECK(guarded == expected);
}
