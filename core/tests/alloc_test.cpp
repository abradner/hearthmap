// No allocation once warmed up (docs/decisions.md D-022): build_view_model() and heat_samples()
// into reused storage must not touch the heap on a steady-state frame.
//
// A separate executable, because it replaces the global operator new to count allocations.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
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

TEST_CASE("a warmed-up build_view_model does not allocate") {
  struct Ctx {
    Config config = flat_config();
    Readings readings = flat_thirsty_many();
    UiState ui{Layer::Soil, std::string("plant_fiddle")};
    ViewModel vm;
  } ctx;
  auto frame = [](void* c) {
    auto* x = static_cast<Ctx*>(c);
    build_view_model(x->config, x->readings, kFlatNow, x->ui, x->vm);
  };
  frame(&ctx);  // warm up
  CHECK(allocations_during(frame, &ctx) == 0);
  // A later frame with new readings (same entities) and another overlay of no more rows.
  ctx.readings = flat_humid_bathroom();
  ctx.ui = UiState{Layer::Humidity, std::string("plant_basil")};
  CHECK(allocations_during(frame, &ctx) == 0);
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
