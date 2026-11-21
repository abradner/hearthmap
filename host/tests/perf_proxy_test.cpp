// The panel's steady-state drawing cost, measured on the host (components/hearthmap/README.md,
// "Measuring"). The panel renders in software on a 360 MHz core, so what LVGL is asked to draw
// per second decides how long its refreshes hold the main loop, and with it how quickly a touch
// is read. That amount does not depend on the CPU: these tests run the real Screen, through the
// host's App (core decides every pulse), for some simulated seconds of an unchanging house with
// pulses running, and count
//   - pixels LVGL flushed (what it re-rendered), per second;
//   - the area of every draw task it created (each task clipped to its refresh area), per second
//     and by task type, which is closer to the software renderer's work: one flushed pixel can
//     carry an image, a line, a label and a mask on top of each other.
// The budgets below are the regression guard; the printed table is the instrument.
//
// Pixels are not all alike, so the budget is a cost model (D-053): each flush, each flushed
// pixel and each pixel of each draw-task type costs what the target measured (TargetRates, one
// table per target, D-054). The panel's table is fitted to its perf logs (ESP32-P4 at 360 MHz,
// real config, c182d6b and 350b09a): about 0.85 ms fixed per flushed area (LVGL's per-area work,
// PPA rotation set-up, the DMA2D copy and its wait), 0.077 us per flushed pixel, and for
// rendering about 0.03 us per pixel of opaque RGB565 image, 0.02 of fill, and 0.55 of the
// anti-aliased shapes (lines, arcs, borders, labels, masks, layers). The budget is 300 ms of
// LVGL per second (30 % of the loop) and 33 ms for any single refresh in steady state.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "headless.h"
#include "json_config.h"
#include "lvgl.h"
#include "src/core/lv_refr_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"

using namespace hearthmap;
using namespace hearthmap::host;

namespace {

/// What one target costs per operation, fitted to its own measurements (D-054).
struct TargetRates {
  const char* name;
  double flush_fixed_ms;                  ///< per flushed area
  double flush_us_per_px;                 ///< per flushed pixel
  std::array<double, 32> task_us_per_px;  ///< by lv_draw_task_type_t
};

TargetRates p4_rates() {
  TargetRates r{"ESP32-P4 1280x800", 0.85, 0.077, {}};
  r.task_us_per_px.fill(0.55);  // anti-aliased shapes and anything not listed
  r.task_us_per_px[LV_DRAW_TASK_TYPE_IMAGE] = 0.03;
  r.task_us_per_px[LV_DRAW_TASK_TYPE_FILL] = 0.02;
  return r;
}

const TargetRates kRates = p4_rates();
constexpr double kBudgetMsPerS = 300.0;
constexpr double kBudgetRefreshMs = 33.0;

// ---- counting ----------------------------------------------------------------------------------

struct Counts {
  std::uint64_t flushed_px = 0;
  std::uint64_t invalidated_px = 0;
  std::uint32_t refreshes = 0;
  std::uint64_t refresh_px = 0;      // flushed in the refresh in progress
  std::uint64_t max_refresh_px = 0;  // the largest single refresh
  std::uint32_t flushes = 0;
  double cost_ms = 0;     // the cost model's total
  double refresh_ms = 0;  // ...for the refresh in progress
  double max_refresh_ms = 0;
  std::uint32_t tasks = 0;
  std::array<std::uint64_t, 32> task_px{};  // by lv_draw_task_type_t
  std::uint64_t task_px_total() const {
    std::uint64_t s = 0;
    for (auto v : task_px) s += v;
    return s;
  }
};

Counts* g_counts = nullptr;  // counting is on while this is set

std::int32_t count_task(lv_draw_unit_t* /*unit*/, lv_draw_task_t* t) {
  if (g_counts == nullptr) return 0;
  lv_area_t a;
  if (!lv_area_intersect(&a, &t->area, &t->clip_area)) return 0;
  const auto type = static_cast<std::size_t>(t->type);
  ++g_counts->tasks;
  if (type < g_counts->task_px.size()) {
    g_counts->task_px[type] += lv_area_get_size(&a);
    const double ms = lv_area_get_size(&a) * kRates.task_us_per_px[type] / 1000.0;
    g_counts->cost_ms += ms;
    g_counts->refresh_ms += ms;
  }
  return 0;  // never takes the task: the software unit still draws it
}

std::int32_t never_dispatch(lv_draw_unit_t* /*unit*/, lv_layer_t* /*layer*/) {
  return LV_DRAW_UNIT_IDLE;
}

// A draw unit that only looks: LVGL asks every unit to evaluate every new task.
void install_task_counter() {
  static bool installed = false;
  if (installed) return;
  auto* unit = static_cast<lv_draw_unit_t*>(lv_draw_create_unit(sizeof(lv_draw_unit_t)));
  unit->evaluate_cb = count_task;
  unit->dispatch_cb = never_dispatch;  // LVGL calls every unit's dispatch_cb unchecked
  unit->name = "PERF_PROBE";
  installed = true;
}

// ESPHome's lvgl component rounds every invalidated area out to its draw rounding (2 px on the
// panel; lvgl_esphome.cpp, rounder_cb), before LVGL decides what covers it. Mirrored here, first.
constexpr std::int32_t kDrawRounding = 2;
void rounder(lv_event_t* e) {
  auto* a = static_cast<lv_area_t*>(lv_event_get_param(e));
  a->x1 = a->x1 / kDrawRounding * kDrawRounding;
  a->y1 = a->y1 / kDrawRounding * kDrawRounding;
  a->x2 = (a->x2 + kDrawRounding) / kDrawRounding * kDrawRounding - 1;
  a->y2 = (a->y2 + kDrawRounding) / kDrawRounding * kDrawRounding - 1;
}

void on_display_event(lv_event_t* e) {
  if (g_counts == nullptr) return;
  const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(e));
  switch (lv_event_get_code(e)) {
    case LV_EVENT_FLUSH_START:
      if (area != nullptr) {
        g_counts->flushed_px += lv_area_get_size(area);
        g_counts->refresh_px += lv_area_get_size(area);
        ++g_counts->flushes;
        const double ms =
            kRates.flush_fixed_ms + lv_area_get_size(area) * kRates.flush_us_per_px / 1000.0;
        g_counts->cost_ms += ms;
        g_counts->refresh_ms += ms;
      }
      break;
    case LV_EVENT_INVALIDATE_AREA:
      if (area != nullptr) g_counts->invalidated_px += lv_area_get_size(area);
      break;
    case LV_EVENT_REFR_START:
      ++g_counts->refreshes;
      g_counts->refresh_px = 0;
      g_counts->refresh_ms = 0;
      break;
    case LV_EVENT_REFR_READY:
      g_counts->max_refresh_px = std::max(g_counts->max_refresh_px, g_counts->refresh_px);
      g_counts->max_refresh_ms = std::max(g_counts->max_refresh_ms, g_counts->refresh_ms);
      break;
    default: break;
  }
}

const char* task_name(std::size_t type) {
  switch (type) {
    case LV_DRAW_TASK_TYPE_FILL: return "fill";
    case LV_DRAW_TASK_TYPE_BORDER: return "border";
    case LV_DRAW_TASK_TYPE_BOX_SHADOW: return "shadow";
    case LV_DRAW_TASK_TYPE_LETTER: return "letter";
    case LV_DRAW_TASK_TYPE_LABEL: return "label";
    case LV_DRAW_TASK_TYPE_IMAGE: return "image";
    case LV_DRAW_TASK_TYPE_LAYER: return "layer";
    case LV_DRAW_TASK_TYPE_LINE: return "line";
    case LV_DRAW_TASK_TYPE_ARC: return "arc";
    case LV_DRAW_TASK_TYPE_TRIANGLE: return "triangle";
    case LV_DRAW_TASK_TYPE_MASK_RECTANGLE: return "mask_rect";
    case LV_DRAW_TASK_TYPE_MASK_BITMAP: return "mask_bitmap";
    default: return "other";
  }
}

// ---- inputs ------------------------------------------------------------------------------------

std::string slurp(const std::string& path) {
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

Scenario panel_like() {
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

Scenario example_thirsty() {
  const std::string dir = std::string(HEARTHMAP_EXAMPLES_DIR) + "/flat/";
  Scenario s;
  s.config = *JsonConfigSource(slurp(dir + "config.json")).load().config;
  const ReadingsFixture fx = *parse_readings(slurp(dir + "readings/thirsty-many.json")).fixture;
  s.readings = fx.readings;
  s.now = fx.now;
  return s;
}

// ---- the run -----------------------------------------------------------------------------------

struct Result {
  double seconds = 0;
  Counts counts;
  double host_ms = 0;  // wall time in lv_timer_handler: informational only
  std::size_t pulsing_pins = 0;
  std::size_t pulsing_rooms = 0;
  double flushed_per_s() const { return static_cast<double>(counts.flushed_px) / seconds; }
  double cost_ms_per_s() const { return counts.cost_ms / seconds; }
  double tasks_per_s() const { return static_cast<double>(counts.task_px_total()) / seconds; }
};

constexpr std::uint32_t kTickMs = 16;  // ESPHome's LV_DEF_REFR_PERIOD

Result run_steady(const char* name, const Scenario& s, Layer layer, double seconds) {
  HeadlessDisplay display(1280, 800);
  lv_display_add_event_cb(display.display(), rounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  install_task_counter();  // after lv_init(), which would forget a unit created before it
  for (lv_event_code_t code :
       {LV_EVENT_FLUSH_START, LV_EVENT_INVALIDATE_AREA, LV_EVENT_REFR_START, LV_EVENT_REFR_READY})
    lv_display_add_event_cb(display.display(), on_display_event, code, nullptr);
  SizedPhotos photos;
  App app(display.screen(), s.config, s.readings, s.now, 0, &photos);
  app.set_layer(layer);

  Result r;
  for (const PinView& pv : app.view_model().pins) {
    if (pv.on_active_layer && pv.active_severity == Severity::Danger) ++r.pulsing_pins;
  }
  for (const RoomView& rv : app.view_model().rooms) {
    if (rv.pulse == Severity::Warn || rv.pulse == Severity::Danger) ++r.pulsing_rooms;
  }

  // Warm up: the first full draw and a second of animation, not counted.
  std::uint32_t t = 0;
  display.render();
  for (; t < 1000; t += kTickMs) {
    display.set_time_ms(t);
    display.run_timers();
  }
  Counts counts;
  g_counts = &counts;
  const auto start = std::chrono::steady_clock::now();
  const std::uint32_t end = t + static_cast<std::uint32_t>(seconds * 1000.0);
  for (; t < end; t += kTickMs) {
    display.set_time_ms(t);
    display.run_timers();
  }
  const auto stop = std::chrono::steady_clock::now();
  g_counts = nullptr;
  r.seconds = seconds;
  r.counts = counts;
  r.host_ms = std::chrono::duration<double, std::milli>(stop - start).count();

  std::printf("perf proxy [%s]: %zu pulsing pins, %zu pulsing rooms, over %.0f s\n", name,
              r.pulsing_pins, r.pulsing_rooms, seconds);
  std::printf(
      "  cost model (%s): %.0f ms/s of LVGL, %.1f flushes/s, largest refresh %.1f ms "
      "(%.1f kpx)\n",
      kRates.name, r.cost_ms_per_s(), counts.flushes / seconds, counts.max_refresh_ms,
      static_cast<double>(counts.max_refresh_px) / 1000.0);
  std::printf(
      "  refreshes %.1f/s, flushed %.0f kpx/s, invalidated %.0f kpx/s, draw tasks %.0f/s "
      "covering %.0f kpx/s, host %.1f ms/s\n",
      counts.refreshes / seconds, r.flushed_per_s() / 1000.0,
      static_cast<double>(counts.invalidated_px) / seconds / 1000.0, counts.tasks / seconds,
      r.tasks_per_s() / 1000.0, r.host_ms / seconds);
  std::printf("  by task type (kpx/s):");
  for (std::size_t i = 0; i < counts.task_px.size(); ++i) {
    if (counts.task_px[i] != 0) {
      std::printf(" %s %.0f", task_name(i),
                  static_cast<double>(counts.task_px[i]) / seconds / 1000.0);
    }
  }
  std::printf("\n");
  return r;
}

}  // namespace

TEST_CASE("perf proxy: the panel-shaped house, pulses running, stays inside its drawing budget") {
  const Result r = run_steady("panel-like, soil layer", panel_like(), Layer::Soil, 10.0);
  // The scenario must actually exercise the pulses, or the budget proves nothing.
  REQUIRE(r.pulsing_pins == 9);
  REQUIRE(r.pulsing_rooms >= 3);
  CHECK(r.counts.flushed_px > 0);  // positive control: the probe sees refreshes at all
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

TEST_CASE("perf proxy: the panel-shaped house on the temperature layer, rooms pulsing") {
  const Result r =
      run_steady("panel-like, temperature layer", panel_like(), Layer::Temperature, 10.0);
  REQUIRE(r.pulsing_rooms >= 4);
  CHECK(r.counts.flushed_px > 0);
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

TEST_CASE("perf proxy: the example flat, thirsty, stays inside its drawing budget") {
  const Result r =
      run_steady("example thirsty-many, soil layer", example_thirsty(), Layer::Soil, 10.0);
  REQUIRE(r.pulsing_pins >= 1);
  CHECK(r.counts.flushed_px > 0);
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

namespace {

/// Pixels LVGL redraws to answer one action: the refreshes in the few ticks after it.
std::uint64_t answer_px(HeadlessDisplay& display, std::uint32_t& t, std::uint64_t& largest,
                        double& largest_ms) {
  Counts counts;
  g_counts = &counts;
  for (int i = 0; i < 4; ++i, t += kTickMs) {
    display.set_time_ms(t);
    display.run_timers();
  }
  g_counts = nullptr;
  largest = counts.max_refresh_px;
  largest_ms = counts.max_refresh_ms;
  return counts.flushed_px;
}

}  // namespace

// Not a budget yet: what answering a tap costs, printed so device numbers can be read against it.
TEST_CASE("perf proxy: what a tap costs to answer (informative)") {
  const Scenario s = panel_like();
  HeadlessDisplay display(1280, 800);
  lv_display_add_event_cb(display.display(), rounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  install_task_counter();
  for (lv_event_code_t code :
       {LV_EVENT_FLUSH_START, LV_EVENT_INVALIDATE_AREA, LV_EVENT_REFR_START, LV_EVENT_REFR_READY})
    lv_display_add_event_cb(display.display(), on_display_event, code, nullptr);
  SizedPhotos photos;
  App app(display.screen(), s.config, s.readings, s.now, 0, &photos);
  app.set_layer(Layer::Soil);
  std::uint32_t t = 0;
  display.render();
  for (; t < 1000; t += kTickMs) {
    display.set_time_ms(t);
    display.run_timers();
  }
  const CanvasRect& a = s.config.canvas.map_area;
  const MapPoint p = s.config.pins[8].position;
  const auto x = static_cast<std::int32_t>(a.x + p.x * a.width);
  const auto y = static_cast<std::int32_t>(a.y + p.y * a.height);
  std::uint64_t largest = 0;
  double ms = 0;
  display.tap(x, y);
  REQUIRE(app.process_events() == 1);
  REQUIRE(app.ui_state().overlay_pin.has_value());
  const std::uint64_t open = answer_px(display, t, largest, ms);
  std::printf(
      "perf proxy [tap answers]: open overlay %.0f kpx (largest refresh %.0f kpx, "
      "%.0f ms)\n",
      static_cast<double>(open) / 1000.0, static_cast<double>(largest) / 1000.0, ms);
  display.tap(20, 780);  // the scrim
  REQUIRE(app.process_events() == 1);
  const std::uint64_t close = answer_px(display, t, largest, ms);
  std::printf(
      "perf proxy [tap answers]: close overlay %.0f kpx (largest refresh %.0f kpx, "
      "%.0f ms)\n",
      static_cast<double>(close) / 1000.0, static_cast<double>(largest) / 1000.0, ms);
  app.set_layer(Layer::Temperature);
  const std::uint64_t tab = answer_px(display, t, largest, ms);
  std::printf(
      "perf proxy [tap answers]: change layer %.0f kpx (largest refresh %.0f kpx, "
      "%.0f ms)\n",
      static_cast<double>(tab) / 1000.0, static_cast<double>(largest) / 1000.0, ms);
  CHECK(open > 0);
}
