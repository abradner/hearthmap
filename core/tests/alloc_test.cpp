// No allocation once warmed up (docs/decisions.md D-022): build_view_model() and heat_samples()
// into reused storage must not touch the heap on a steady-state frame.
//
// A separate executable, because it replaces the global operator new to count allocations.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "flat_fixture.h"
#include "hearthmap/view_model.h"

namespace {
bool g_counting = false;
std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
  if (g_counting) ++g_allocations;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}

using namespace hearthmap;
using namespace hearthmap::test;

namespace {
std::size_t allocations_during(void (*fn)(void*), void* ctx) {
  g_allocations = 0;
  g_counting = true;
  fn(ctx);
  g_counting = false;
  return g_allocations;
}
}  // namespace

TEST_CASE("the counter sees allocations (positive control)") {
  struct Ctx {
    std::vector<int> v;
  } ctx;
  CHECK(allocations_during([](void* c) { static_cast<Ctx*>(c)->v.resize(100); }, &ctx) > 0);
}

// The contract (view_model.h, build_view_model): no allocation once warm while the overlay
// state is unchanged. Opening or switching an overlay may allocate, so nothing is asserted
// about the frame on which overlay_pin changes.
struct VmCtx {
  Config config = flat_config();
  Readings readings = flat_thirsty_many();
  UiState ui{Layer::Soil, std::nullopt};
  ViewModel vm;
};

void vm_frame(void* c) {
  auto* x = static_cast<VmCtx*>(c);
  build_view_model(x->config, x->readings, kFlatNow, x->ui, x->vm);
}

TEST_CASE("a warmed-up build_view_model with no overlay does not allocate") {
  VmCtx ctx;
  vm_frame(&ctx);  // warm up
  CHECK(allocations_during(vm_frame, &ctx) == 0);
  // New readings (same entities) and another layer, still no overlay.
  ctx.readings = flat_humid_bathroom();
  ctx.ui.active_layer = Layer::Humidity;
  CHECK(allocations_during(vm_frame, &ctx) == 0);
}

TEST_CASE("a warmed-up build_view_model with the same overlay open does not allocate") {
  VmCtx ctx;
  ctx.ui.overlay_pin = std::string("plant_fiddle");
  vm_frame(&ctx);  // warm up, overlay open
  REQUIRE(ctx.vm.overlay.has_value());
  CHECK(allocations_during(vm_frame, &ctx) == 0);
  // New readings and another layer; the same overlay stays open.
  ctx.readings = flat_humid_bathroom();
  ctx.ui.active_layer = Layer::Humidity;
  CHECK(allocations_during(vm_frame, &ctx) == 0);
  REQUIRE(ctx.vm.overlay.has_value());
}

TEST_CASE("a closed overlay stays allocation-free on later frames") {
  VmCtx ctx;
  ctx.ui.overlay_pin = std::string("plant_fiddle");
  vm_frame(&ctx);
  ctx.ui.overlay_pin.reset();
  vm_frame(&ctx);  // the closing frame: not asserted
  CHECK_FALSE(ctx.vm.overlay.has_value());
  CHECK(allocations_during(vm_frame, &ctx) == 0);
}

TEST_CASE("warmed up on healthy readings, plants turning thirsty does not allocate") {
  // The HUD's storage must not depend on how many plants were thirsty during warm-up.
  for (const bool overlay : {false, true}) {
    INFO("overlay open: " << overlay);
    VmCtx ctx;
    ctx.readings = flat_all_ok();
    if (overlay) ctx.ui.overlay_pin = std::string("plant_fiddle");
    vm_frame(&ctx);  // warm up: HUD empty
    REQUIRE(ctx.vm.hud.items.empty());
    CHECK(allocations_during(vm_frame, &ctx) == 0);
    ctx.readings = flat_thirsty_many();
    CHECK(allocations_during(vm_frame, &ctx) == 0);
    CHECK_FALSE(ctx.vm.hud.items.empty());
  }
}

TEST_CASE("same overlay open, a missing reading arrives with a unit within capacity") {
  REQUIRE(kOverlayUnitCapacity == 16);
  VmCtx ctx;
  ctx.readings = flat_all_ok();
  ctx.readings.by_entity.erase("sensor.example_fiddle_battery");
  ctx.ui.overlay_pin = std::string("plant_fiddle");
  vm_frame(&ctx);  // warm up with the battery row empty
  CHECK(allocations_during(vm_frame, &ctx) == 0);
  // The reading arrives with a unit of exactly the capacity: longer than libstdc++'s inline
  // string buffer (15), so an unreserved row would allocate here.
  Reading battery;
  battery.state = ReadingState::Value;
  battery.value = 80;
  battery.unit = std::string(kOverlayUnitCapacity, 'u');
  battery.last_updated = kFlatNow;
  ctx.readings.by_entity["sensor.example_fiddle_battery"] = battery;
  CHECK(allocations_during(vm_frame, &ctx) == 0);
  bool shown = false;
  for (const auto& row : ctx.vm.overlay->rows) {
    if (row.assessment.role == Role::Battery) {
      shown = true;
      CHECK(row.unit == battery.unit);
    }
  }
  CHECK(shown);
}

TEST_CASE("a unit longer than the capacity is still shown in full") {
  // Allowed to allocate (view_model.h); only correctness is checked.
  VmCtx ctx;
  ctx.readings = flat_all_ok();
  ctx.ui.overlay_pin = std::string("plant_fiddle");
  vm_frame(&ctx);
  const std::string long_unit(3 * kOverlayUnitCapacity, 'w');
  ctx.readings.by_entity["sensor.example_fiddle_battery"].unit = long_unit;
  vm_frame(&ctx);
  bool shown = false;
  for (const auto& row : ctx.vm.overlay->rows) {
    if (row.assessment.role == Role::Battery) {
      shown = true;
      CHECK(row.unit == long_unit);
    }
  }
  CHECK(shown);
}

TEST_CASE("a warmed-up heat_samples does not allocate") {
  struct Ctx {
    Config config = flat_config();
    Readings readings = flat_all_ok();
    std::vector<HeatSample> out;
  } ctx;
  auto frame = [](void* c) {
    auto* x = static_cast<Ctx*>(c);
    heat_samples(x->config, x->readings, kFlatNow, Layer::Temperature, x->out);
  };
  frame(&ctx);
  CHECK(allocations_during(frame, &ctx) == 0);
  CHECK(ctx.out.size() == 7);
}
