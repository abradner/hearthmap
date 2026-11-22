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
//
// The panel shows pulse frames through its presenter (D-058): a frame whose tile nothing covers
// goes to the panel straight from its precomputed buffer, outside LVGL's refresh, and only the
// rest are invalidated. The proxy runs that path as the panel does (Screen::show_due_frames after
// every timer pass) with a presenter that accepts and charges the target's own present rate:
// 0.06 ms fixed plus 0.027 us per pixel, fitted to perf-hw's bench (0.17 ms at 64 x 64, 0.31 at
// 96 x 96, 0.50 at 128 x 128: the PPA rotation into the frame buffer and its cache write-back).
// The fallback path (no presenter) is printed for comparison, not budgeted.
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
#include "scenarios.h"
#include "src/core/lv_refr_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"

using namespace hearthmap;
using namespace hearthmap::host;
using namespace hearthmap::host::test;

namespace {

/// What one target costs per operation, fitted to its own measurements (D-054).
struct TargetRates {
  const char* name;
  double flush_fixed_ms;                  ///< per flushed area
  double flush_us_per_px;                 ///< per flushed pixel
  std::array<double, 32> task_us_per_px;  ///< by lv_draw_task_type_t
  double present_fixed_ms;                ///< per presented tile (D-058)
  double present_us_per_px;               ///< per presented pixel
  std::size_t pulse_memory_bytes;         ///< the pulse sprites' budget (D-054)
};

TargetRates p4_rates() {
  TargetRates r{"ESP32-P4 1280x800", 0.85, 0.077, {}, 0.06, 0.027, kPanelPulseMemory};
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
  std::uint32_t presented = 0;              // pulse frames sent straight to the panel
  std::uint32_t fallbacks = 0;              // ...and invalidated instead
  std::array<std::uint32_t, 16> blocked{};  // fallbacks by ui::PresentBlock
  double present_ms = 0;                    // the cost model's share for presenting
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

// ---- the run -----------------------------------------------------------------------------------

/// How pulse frames reach the panel.
enum class FramePath : std::uint8_t {
  Present,     ///< through a presenter where nothing covers the tile, as the panel runs
  Invalidate,  ///< every frame invalidated and repainted by LVGL
};

/// Accepts every frame and charges the target's present rate for it.
class ChargingPresenter final : public ui::Presenter {
 public:
  bool present(int x, int y, const ui::View565& tile) override {
    (void)x;
    (void)y;
    if (g_counts != nullptr) {
      const double ms = kRates.present_fixed_ms +
                        static_cast<double>(tile.w) * tile.h * kRates.present_us_per_px / 1000.0;
      g_counts->present_ms += ms;
      g_counts->cost_ms += ms;
    }
    return true;
  }
};

struct Result {
  double seconds = 0;
  Counts counts;
  double host_ms = 0;  // wall time in lv_timer_handler: informational only
  std::size_t pulsing_pins = 0;
  std::size_t pulsing_rooms = 0;
  ui::PulseCounters pulses;  // at the end of the run
  double flushed_per_s() const { return static_cast<double>(counts.flushed_px) / seconds; }
  double cost_ms_per_s() const { return counts.cost_ms / seconds; }
  double tasks_per_s() const { return static_cast<double>(counts.task_px_total()) / seconds; }
};

constexpr std::uint32_t kTickMs = 16;  // ESPHome's LV_DEF_REFR_PERIOD

Result run_steady(const char* name, const Scenario& s, Layer layer, double seconds,
                  FramePath path = FramePath::Present) {
  HeadlessDisplay display(1280, 800);
  lv_display_add_event_cb(display.display(), rounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  install_task_counter();  // after lv_init(), which would forget a unit created before it
  for (lv_event_code_t code :
       {LV_EVENT_FLUSH_START, LV_EVENT_INVALIDATE_AREA, LV_EVENT_REFR_START, LV_EVENT_REFR_READY})
    lv_display_add_event_cb(display.display(), on_display_event, code, nullptr);
  SizedPhotos photos;
  App app(display.screen(), s.config, s.readings, s.now, 0, &photos, kRates.pulse_memory_bytes);
  app.set_layer(layer);
  ChargingPresenter presenter;
  const bool presenting = path == FramePath::Present;
  app.screen().set_frames_deferred(presenting);
  // One timer pass, then (presenting) the frames it chose, outside LVGL's refresh as the panel's
  // main loop shows them.
  auto step = [&](std::uint32_t at, Counts* c) {
    display.set_time_ms(at);
    display.run_timers();
    if (!presenting) return;
    ui::Screen::ShownFrames shown;
    app.screen().show_due_frames(presenter, &shown);
    if (c != nullptr) {
      c->presented += shown.presented;
      c->fallbacks += shown.invalidated;
      for (std::size_t i = 0; i < shown.blocked.size(); ++i) c->blocked[i] += shown.blocked[i];
    }
  };

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
  for (; t < 1000; t += kTickMs) step(t, nullptr);
  Counts counts;
  g_counts = &counts;
  const auto start = std::chrono::steady_clock::now();
  const std::uint32_t end = t + static_cast<std::uint32_t>(seconds * 1000.0);
  for (; t < end; t += kTickMs) step(t, &counts);
  const auto stop = std::chrono::steady_clock::now();
  g_counts = nullptr;
  r.pulses = app.screen().pulse_counters();
  r.seconds = seconds;
  r.counts = counts;
  r.host_ms = std::chrono::duration<double, std::milli>(stop - start).count();

  std::printf("perf proxy [%s, %s]: %zu pulsing pins, %zu pulsing rooms, over %.0f s\n", name,
              presenting ? "presenting" : "invalidating", r.pulsing_pins, r.pulsing_rooms, seconds);
  std::printf("  sprites: %zu rings and %zu outlines animated, frames / %u, %.1f MiB\n",
              r.pulses.rings_shown, r.pulses.rooms_animated, r.pulses.frame_divisor,
              static_cast<double>(r.pulses.tile_bytes) / (1024.0 * 1024.0));
  if (presenting) {
    std::printf("  presented %.1f frames/s (%.0f ms/s of it), fell back %.1f/s\n",
                counts.presented / seconds, counts.present_ms / seconds,
                counts.fallbacks / seconds);
    std::printf("  fallbacks by PresentBlock:");
    for (std::size_t i = 0; i < counts.blocked.size(); ++i) {
      if (counts.blocked[i] != 0) std::printf(" %zu: %u", i, counts.blocked[i]);
    }
    std::printf("\n");
  }
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
  // Positive control: the frames reach the panel (presented; nothing needs LVGL to repaint, so
  // a steady house may flush nothing at all).
  CHECK(r.counts.presented + r.counts.fallbacks > 0);
  CHECK(r.counts.fallbacks * 10U <= r.counts.presented);  // outline cells and rings present
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

TEST_CASE("perf proxy: the panel-shaped house on the temperature layer, rooms pulsing") {
  const Result r =
      run_steady("panel-like, temperature layer", panel_like(), Layer::Temperature, 10.0);
  REQUIRE(r.pulsing_rooms >= 4);
  CHECK(r.counts.presented + r.counts.fallbacks > 0);
  CHECK(r.counts.fallbacks * 10U <= r.counts.presented);
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

TEST_CASE("perf proxy: pins next to walls of pulsing neighbours present without covered tiles") {
  const Result r = run_steady("edge-heavy, soil layer", edge_heavy(), Layer::Soil, 10.0);
  REQUIRE(r.pulsing_pins == 9);
  REQUIRE(r.pulsing_rooms >= 3);
  CHECK(r.counts.presented > 0);
  CHECK(r.counts.blocked[static_cast<std::size_t>(ui::PresentBlock::Covered)] == 0U);
  CHECK(r.counts.fallbacks * 10U <= r.counts.presented);
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
}

// After a heat refresh every sprite is recaptured. On the panel (3bcd667) a full recompose of
// the busiest tab took about 43 s at one unit per loop pass, longer than the 15 s heat interval,
// so the tab never settled. A refresh that leaves the map under a sprite as it was keeps that
// sprite's frames (D-071): two captures per tile, nothing composed.
TEST_CASE("perf proxy: a heat refresh that changes nothing under the sprites settles at once") {
  const Scenario s = panel_like();
  HeadlessDisplay display(1280, 800);
  SizedPhotos photos;
  App app(display.screen(), s.config, s.readings, s.now, 0, &photos, kRates.pulse_memory_bytes);
  app.set_layer(Layer::Soil);
  auto drain = [&] {
    std::uint32_t steps = 0;
    while (app.screen().prepare_step()) ++steps;
    return steps;
  };
  (void)drain();
  const ui::PulseCounters before = app.screen().pulse_counters();
  REQUIRE(before.rings_shown == 9);
  app.screen().invalidate_heat();  // the same image: nothing under any sprite moved
  const std::uint32_t same_steps = drain();
  const ui::PulseCounters same = app.screen().pulse_counters();
  const std::uint32_t tiles = same.captures - before.captures;
  // A changed image (every reading moved a little): sprites are composed again, the positive
  // control that the comparison is not simply always "unchanged".
  Readings moved = s.readings;
  for (auto& [entity, r] : moved.by_entity) {
    if (r.state == ReadingState::Value) r.value += 3.0;
  }
  app.set_readings(moved, s.now);  // rebuilds the image and drains preparation
  const ui::PulseCounters changed = app.screen().pulse_counters();
  std::printf(
      "perf proxy [settle]: %u tiles recaptured; unchanged image: %u steps, %u sets kept, %u "
      "built; changed image: %u sets built, %u kept\n",
      tiles, same_steps, same.sets_kept - before.sets_kept, same.sets_built - before.sets_built,
      changed.sets_built - same.sets_built, changed.sets_kept - same.sets_kept);
  CHECK(tiles > 9U);
  CHECK(same.sets_built == before.sets_built);  // nothing composed
  CHECK(same.sets_kept - before.sets_kept == tiles);
  CHECK(same_steps <= 2U * tiles + 1U);  // two captures each, then idle
  CHECK(changed.sets_built - same.sets_built >= tiles / 2U);
}

TEST_CASE("perf proxy: the example flat, thirsty, stays inside its drawing budget") {
  const Result r =
      run_steady("example thirsty-many, soil layer", example_thirsty(), Layer::Soil, 10.0);
  REQUIRE(r.pulsing_pins >= 1);
  CHECK(r.counts.presented + r.counts.fallbacks > 0);
  CHECK(r.counts.fallbacks * 10U <= r.counts.presented);
  CHECK(r.cost_ms_per_s() <= kBudgetMsPerS);
  CHECK(r.counts.max_refresh_ms <= kBudgetRefreshMs);
}

// Not a budget: the same house with every frame invalidated (a target with no presenter), so the
// present path's saving can be read off.
TEST_CASE("perf proxy: the panel-shaped house without a presenter (informative)") {
  const Result r =
      run_steady("panel-like, soil layer", panel_like(), Layer::Soil, 10.0, FramePath::Invalidate);
  CHECK(r.counts.flushed_px > 0);
  CHECK(r.counts.presented == 0);
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
  std::printf("    drawn (kpx, modelled ms):");
  for (std::size_t i = 0; i < counts.task_px.size(); ++i) {
    if (counts.task_px[i] == 0) continue;
    std::printf(" %s %.0f/%.0f", task_name(i), static_cast<double>(counts.task_px[i]) / 1000.0,
                static_cast<double>(counts.task_px[i]) * kRates.task_us_per_px[i] / 1000.0);
  }
  std::printf("; %u flushes\n", counts.flushes);
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
