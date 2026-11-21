#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "headless.h"
#include "hearthmap/ui/present.h"
#include "lvgl_private.h"

using namespace hearthmap;
using namespace hearthmap::ui;

namespace {

struct RecordingPresenter final : Presenter {
  bool accept = true;
  int calls = 0;
  int x = -1, y = -1;
  View565 last{};
  bool present(int px, int py, const View565& tile) override {
    ++calls;
    x = px;
    y = py;
    last = tile;
    return accept;
  }
};

constexpr int kT = 16;

struct Tile {
  std::vector<std::uint16_t> a = std::vector<std::uint16_t>(kT * kT, 0xF800);
  std::vector<std::uint16_t> b = std::vector<std::uint16_t>(kT * kT, 0x001F);
  lv_image_dsc_t dsc{};
  lv_obj_t* obj = nullptr;
  Tile(lv_obj_t* parent, int x, int y) {
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = kT;
    dsc.header.h = kT;
    dsc.header.stride = kT * 2;
    dsc.data_size = kT * kT * 2;
    dsc.data = reinterpret_cast<const std::uint8_t*>(a.data());
    obj = lv_image_create(parent);
    lv_image_set_src(obj, &dsc);
    lv_obj_set_pos(obj, x, y);
  }
};

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  return o;
}

void clean(host::HeadlessDisplay& d) {
  lv_obj_clean(d.screen());
  lv_obj_clean(lv_display_get_layer_top(d.display()));
  d.render();
}

}  // namespace

TEST_CASE(
    "present: a clean opaque tile is presented, and LVGL's next repaint shows the same frame") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  Tile t(d.screen(), 30, 20);
  d.render();
  RecordingPresenter p;
  PresentBlock why = PresentBlock::NotImage;
  CHECK(show_tile_frame(t.obj, t.dsc, t.b.data(), p, &why) == TileShown::Presented);
  CHECK(why == PresentBlock::Ok);
  CHECK(p.calls == 1);
  CHECK(p.x == 30);
  CHECK(p.y == 20);
  CHECK(p.last.px == t.b.data());
  CHECK(p.last.w == kT);
  CHECK(p.last.stride == kT);
  CHECK(t.dsc.data == reinterpret_cast<const std::uint8_t*>(t.b.data()));
  // Nothing was invalidated by the present...
  CHECK(lv_display_get_screen_active(d.display()) == d.screen());
  CHECK(d.display()->inv_p == 0);
  // ...and a repaint for any other reason draws the new frame, not the old one.
  lv_obj_invalidate(d.screen());
  d.render();
  CHECK(d.framebuffer()[static_cast<std::size_t>(25) * 200 + 35] == 0x001F);
}

TEST_CASE("present: a refused present falls back to invalidating the tile") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  Tile t(d.screen(), 30, 20);
  d.render();
  RecordingPresenter p;
  p.accept = false;
  CHECK(show_tile_frame(t.obj, t.dsc, t.b.data(), p) == TileShown::Invalidated);
  CHECK(p.calls == 1);
  CHECK(d.display()->inv_p > 0);
  d.render();
  CHECK(d.framebuffer()[static_cast<std::size_t>(25) * 200 + 35] == 0x001F);
}

TEST_CASE("present: NoPresenter always falls back") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  Tile t(d.screen(), 30, 20);
  d.render();
  NoPresenter none;
  CHECK(show_tile_frame(t.obj, t.dsc, t.b.data(), none) == TileShown::Invalidated);
  CHECK(d.display()->inv_p > 0);
}

TEST_CASE(
    "present: something drawn after the tile and over it blocks; something beneath does not") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  lv_obj_t* under = box(d.screen(), 25, 15, 40, 40);  // created first: drawn beneath
  lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
  Tile t(d.screen(), 30, 20);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
  lv_obj_t* over = box(d.screen(), 40, 30, 10, 10);  // created after: drawn on top
  RecordingPresenter p;
  PresentBlock why = PresentBlock::Ok;
  CHECK(show_tile_frame(t.obj, t.dsc, t.b.data(), p, &why) == TileShown::Invalidated);
  CHECK(why == PresentBlock::Covered);
  CHECK(p.calls == 0);
  // Moved clear of the tile, it no longer blocks.
  lv_obj_set_pos(over, 100, 80);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
  // A hidden one never blocks.
  lv_obj_set_pos(over, 40, 30);
  lv_obj_set_hidden(over, true);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
}

TEST_CASE("present: a later object's shadow reaching into the tile blocks") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  Tile t(d.screen(), 30, 20);
  lv_obj_t* near = box(d.screen(), 30 + kT + 4, 20, 10, 10);  // 4 px clear of the tile
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
  lv_obj_set_style_shadow_width(near, 20, 0);
  lv_obj_set_style_shadow_opa(near, LV_OPA_COVER, 0);
  lv_obj_refresh_ext_draw_size(near);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Covered);
}

TEST_CASE("present: anything on the top layer over the tile blocks") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  Tile t(d.screen(), 30, 20);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
  box(lv_display_get_layer_top(d.display()), 35, 25, 5, 5);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Covered);
  clean(d);
}

TEST_CASE("present: hidden, translucent, transformed, padded, clipped, parent, wrong source") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  lv_obj_t* parent = box(d.screen(), 10, 10, 100, 80);
  Tile t(parent, 20, 10);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);

  lv_obj_set_hidden(parent, true);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Hidden);
  lv_obj_set_hidden(parent, false);

  lv_obj_set_style_opa(parent, LV_OPA_50, 0);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::NotOpaque);
  lv_obj_set_style_opa(parent, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor_opa(t.obj, LV_OPA_30, 0);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::NotOpaque);
  lv_obj_set_style_image_recolor_opa(t.obj, LV_OPA_TRANSP, 0);

  lv_image_set_scale(t.obj, 512);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Transformed);
  lv_image_set_scale(t.obj, LV_SCALE_NONE);
  lv_obj_set_style_transform_rotation(parent, 100, 0);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Transformed);
  lv_obj_set_style_transform_rotation(parent, 0, 0);

  lv_obj_set_style_pad_all(t.obj, 2, 0);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::SizeMismatch);
  lv_obj_set_style_pad_all(t.obj, 0, 0);
  lv_obj_set_size(t.obj, kT + 4, kT);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::SizeMismatch);
  lv_obj_set_size(t.obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);

  lv_obj_set_pos(t.obj, 100 - kT / 2, 10);  // half outside the parent
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Clipped);
  lv_obj_set_overflow_visible(parent, true);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
  lv_obj_set_pos(t.obj, 195, 10);  // off the display, overflow or not
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Clipped);
  lv_obj_set_pos(t.obj, 20, 10);

  lv_obj_t* child = lv_obj_create(t.obj);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::HasChildren);
  lv_obj_delete(child);

  lv_image_dsc_t other = t.dsc;
  CHECK(present_check(t.obj, other) == PresentBlock::NotImage);
  CHECK(present_check(nullptr, t.dsc) == PresentBlock::NotImage);
  CHECK(present_check(parent, t.dsc) == PresentBlock::NotImage);
  t.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::NotRgb565);
  t.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::Ok);
}

TEST_CASE("present: a tile on a screen that is not shown is not presented") {
  host::HeadlessDisplay d(200, 120);
  clean(d);
  lv_obj_t* other = lv_obj_create(nullptr);
  Tile t(other, 30, 20);
  CHECK(present_check(t.obj, t.dsc) == PresentBlock::NotOnActiveScreen);
  lv_obj_delete(other);
}
