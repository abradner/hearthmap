// The map bake's handling of a band that fails to render (D-055).
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "headless.h"
#include "hearthmap/ui/memory.h"
#include "lvgl.h"
#include "map_bake.h"
#include "sprites.h"

using hearthmap::host::HeadlessDisplay;
using hearthmap::ui::detail::MapBake;

namespace {

int g_failures_left = 0;  // calls of flaky_render() on the wall's band that fail first

bool flaky_render(lv_obj_t* o, const lv_area_t& a, std::uint16_t* out, std::uint16_t bg) {
  const bool wall_band = a.y1 <= 120 && a.y2 >= 120;  // the wall is at screen row 120
  if (wall_band && g_failures_left > 0) {
    --g_failures_left;
    return false;
  }
  return hearthmap::ui::detail::render_area(o, a, out, bg);
}

bool always_fails(lv_obj_t*, const lv_area_t&, std::uint16_t*, std::uint16_t) {
  return false;
}

// A 64 x 40 map with a white wall line across it, at (100, 100).
struct Scene {
  HeadlessDisplay display{320, 240};
  lv_obj_t* map = nullptr;
  lv_obj_t* wall = nullptr;
  lv_area_t area{100, 100, 163, 139};
  lv_point_precise_t pts[2] = {{0, 20}, {63, 20}};
  Scene() {
    map = lv_obj_create(display.screen());
    lv_obj_remove_style_all(map);
    lv_obj_set_pos(map, 100, 100);
    lv_obj_set_size(map, 64, 40);
    wall = lv_line_create(map);
    lv_line_set_points(wall, pts, 2);
    lv_obj_set_style_line_width(wall, 3, 0);
    lv_obj_set_style_line_color(wall, lv_color_white(), 0);
    lv_obj_update_layout(display.screen());
  }
  std::vector<std::uint16_t> baked(MapBake& bake) {
    bake.start();
    for (int guard = 0; guard < 1000 && bake.prepare_step(); ++guard) {
    }
    std::vector<std::uint16_t> img(64U * 40U, 0x001F);  // blue under the walls
    if (!bake.apply(img.data(), 64, 0, 40)) img.clear();
    return img;
  }
};

}  // namespace

TEST_CASE("a bake band that fails is rendered again, never published empty") {
  Scene clean;
  MapBake reference(clean.map, clean.area, {clean.wall}, {});
  const std::vector<std::uint16_t> want = clean.baked(reference);
  REQUIRE(reference.ready());
  REQUIRE_FALSE(want.empty());

  Scene s;
  MapBake bake(s.map, s.area, {s.wall}, {});
  bake.set_renderer(flaky_render);
  g_failures_left = 2;  // two failures in a row: below the limit
  const std::vector<std::uint16_t> got = s.baked(bake);
  CHECK(bake.ready());
  CHECK_FALSE(bake.failed());
  CHECK(g_failures_left == 0);  // both injected failures happened
  // The reference drew the wall (not just blue everywhere), so the comparison means something.
  CHECK(want[20U * 64U + 32U] != 0x001F);
  CHECK(got == want);  // the wall is in every band, the failed one included
}

TEST_CASE("a bake whose bands keep failing gives up and publishes nothing") {
  Scene s;
  MapBake bake(s.map, s.area, {s.wall}, {});
  bake.set_renderer(always_fails);
  const std::vector<std::uint16_t> got = s.baked(bake);
  CHECK(bake.failed());
  CHECK_FALSE(bake.ready());
  CHECK(got.empty());  // apply() refuses: the walls stay LVGL objects
}

namespace {

/// The system heap, refusing every block smaller than `below` bytes: the bake's pixel buffers
/// fit, its small pointer lists do not.
struct RefusingSmall final : hearthmap::ui::BulkMemory {
  std::size_t below = 512;
  void* allocate(std::size_t bytes, std::size_t align) override {
    if (bytes < below) return nullptr;
    return hearthmap::ui::system_bulk_memory().allocate(bytes, align);
  }
  void deallocate(void* p, std::size_t bytes) override {
    hearthmap::ui::system_bulk_memory().deallocate(p, bytes);
  }
};

}  // namespace

TEST_CASE("a bake whose working lists the port refuses fails softly, objects left as they are") {
  RefusingSmall mem;
  REQUIRE(hearthmap::ui::set_bulk_memory(&mem));
  {
    // A rebake: the wall is hidden (the shown image carries it), and something over the map
    // must be hidden for each band. Both go on the bake's working lists.
    Scene s;
    lv_obj_t* over = lv_obj_create(s.map);
    lv_obj_set_hidden(s.wall, true);
    MapBake bake(s.map, s.area, {s.wall}, {over});
    const std::vector<std::uint16_t> got = s.baked(bake);  // aborted here before the fix
    CHECK(bake.failed());
    CHECK_FALSE(bake.ready());
    CHECK(got.empty());
    CHECK(lv_obj_is_hidden(s.wall));  // left as it was
    CHECK_FALSE(lv_obj_is_hidden(over));
  }
  CHECK(hearthmap::ui::bulk_stats().in_use == 0);
  REQUIRE(hearthmap::ui::set_bulk_memory(nullptr));
}
