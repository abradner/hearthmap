// What the UI takes from the default heap while it runs (docs/decisions.md D-064).
//
// On the panel, ESP-IDF's CONFIG_SPIRAM_USE_MALLOC sends every malloc() and `new` under 16 KiB to
// internal RAM first, and internal RAM is also where the hosted Wi-Fi driver must find its DMA
// frames. A UI that keeps making 4-12 KiB buffers (sprite captures, coverage tables, frames)
// fills it, and the Wi-Fi link dies with nothing in the log. So the UI's bulk storage goes
// through the embedder's memory port (hearthmap/ui/memory.h), PSRAM on the panel, and this test
// holds what is left on the default heap to a small fixed budget: this executable replaces the
// global operator new and delete with counting ones, runs the panel-shaped house through a long
// walk (every tab, pulses starting and stopping, heat images rebuilt, the overlay opened and
// closed), and checks that after the warm-up
//   - the default heap's bytes in use never rise more than kGrowthBudget above where they stood,
//   - no single default-heap allocation is larger than kLargestAllowed, and
//   - the bulk went through the port instead (a positive control: the walk did real work).
// The probe's own positive control: a 64 KiB vector made inside the counted window is seen.
//
// Set HEARTHMAP_HEAP_TRACE=1 to print a backtrace for every allocation over kLargestAllowed.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define HEARTHMAP_HAVE_BACKTRACE 1
#endif

#include "app.h"
#include "headless.h"
#include "hearthmap/ui/memory.h"
#include "hearthmap/ui/worker_thread.h"
#include "scenarios.h"

namespace {

// ---- the counting default heap ------------------------------------------------------------------

constexpr std::size_t kLargestAllowed = 1024;
constexpr std::size_t kGrowthBudget = 16U * 1024U;
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL's default: blocks this size or smaller stay in internal RAM.
constexpr std::size_t kInternalClass = 16U * 1024U;

struct HeapCounter {
  std::atomic<std::size_t> in_use{0};
  std::atomic<std::size_t> peak{0};
  std::atomic<std::size_t> largest{0};  ///< while counting
  std::atomic<std::size_t> over{0};     ///< allocations over kLargestAllowed while counting
  std::atomic<std::size_t> over_bytes{0};
  std::atomic<std::size_t> small_in_use{0};  ///< in blocks of kInternalClass or less
  std::atomic<std::size_t> small_peak{0};
  std::atomic<bool> counting{false};
  bool trace = false;
};

HeapCounter& counter() {
  static HeapCounter c;  // trivially destructible members: safe to use during static teardown
  return c;
}

struct Header {
  std::size_t size;
  void* raw;
};

void note_alloc(std::size_t n) {
  HeapCounter& c = counter();
  const std::size_t now = c.in_use.fetch_add(n, std::memory_order_relaxed) + n;
  const std::size_t small =
      n <= kInternalClass ? c.small_in_use.fetch_add(n, std::memory_order_relaxed) + n : 0;
  if (!c.counting.load(std::memory_order_relaxed)) return;
  std::size_t sp = c.small_peak.load(std::memory_order_relaxed);
  while (small > sp && !c.small_peak.compare_exchange_weak(sp, small, std::memory_order_relaxed)) {
  }
  std::size_t p = c.peak.load(std::memory_order_relaxed);
  while (now > p && !c.peak.compare_exchange_weak(p, now, std::memory_order_relaxed)) {
  }
  std::size_t l = c.largest.load(std::memory_order_relaxed);
  while (n > l && !c.largest.compare_exchange_weak(l, n, std::memory_order_relaxed)) {
  }
  if (n > kLargestAllowed) {
    c.over.fetch_add(1, std::memory_order_relaxed);
    c.over_bytes.fetch_add(n, std::memory_order_relaxed);
#ifdef HEARTHMAP_HAVE_BACKTRACE
    if (c.trace) {
      c.counting.store(false);  // the trace itself allocates
      void* frames[16];
      const int k = backtrace(frames, 16);
      std::fprintf(stderr, "default heap: %zu bytes from\n", n);
      backtrace_symbols_fd(frames, k, 2);
      c.counting.store(true);
    }
#endif
  }
}

void* counted_new(std::size_t n, std::size_t align, bool nothrow) {
  align = std::max<std::size_t>(align, 2 * sizeof(Header));
  void* raw = nullptr;
  if (posix_memalign(&raw, align, n + align) != 0) raw = nullptr;
  if (raw == nullptr) {
    if (nothrow) return nullptr;
    std::abort();  // exceptions are off (D-002): what operator new does on the panel
  }
  auto* p = static_cast<unsigned char*>(raw) + align;
  reinterpret_cast<Header*>(p)[-1] = Header{n, raw};
  note_alloc(n);
  return p;
}

void counted_delete(void* p) noexcept {
  if (p == nullptr) return;
  const Header h = static_cast<Header*>(p)[-1];
  counter().in_use.fetch_sub(h.size, std::memory_order_relaxed);
  if (h.size <= kInternalClass) counter().small_in_use.fetch_sub(h.size, std::memory_order_relaxed);
  std::free(h.raw);
}

// ---- the port, counted
// ---------------------------------------------------------------------------

/// The system's memory, with a tally: what the UI asked of the port.
class CountingBulk final : public hearthmap::ui::BulkMemory {
 public:
  void* allocate(std::size_t bytes, std::size_t align) override {
    void* p = hearthmap::ui::system_bulk_memory().allocate(bytes, align);
    if (p != nullptr) {
      in_use += bytes;
      peak = std::max(peak, in_use);
      ++allocations;
    }
    return p;
  }
  void deallocate(void* p, std::size_t bytes) override {
    if (p == nullptr) return;
    in_use -= bytes;
    hearthmap::ui::system_bulk_memory().deallocate(p, bytes);
  }
  std::size_t in_use = 0, peak = 0, allocations = 0;
};

/// Takes every frame (the panel's presenter path) and reads nothing.
class Accepting final : public hearthmap::ui::Presenter {
 public:
  bool present(int, int, const hearthmap::ui::View565&) override { return true; }
};

}  // namespace

// The replaceable global allocation functions ([new.delete]): every `new` in this executable,
// core's, ui_lvgl's and the host's included, comes through here.
void* operator new(std::size_t n) {
  return counted_new(n, alignof(std::max_align_t), false);
}
void* operator new[](std::size_t n) {
  return counted_new(n, alignof(std::max_align_t), false);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return counted_new(n, alignof(std::max_align_t), true);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return counted_new(n, alignof(std::max_align_t), true);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return counted_new(n, static_cast<std::size_t>(a), false);
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return counted_new(n, static_cast<std::size_t>(a), false);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_new(n, static_cast<std::size_t>(a), true);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_new(n, static_cast<std::size_t>(a), true);
}
void operator delete(void* p) noexcept {
  counted_delete(p);
}
void operator delete[](void* p) noexcept {
  counted_delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
  counted_delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  counted_delete(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  counted_delete(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  counted_delete(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  counted_delete(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  counted_delete(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  counted_delete(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  counted_delete(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  counted_delete(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  counted_delete(p);
}

using namespace hearthmap;
using namespace hearthmap::host;
using namespace hearthmap::host::test;

namespace {

void long_walk(hearthmap::ui::Worker* worker) {
  HeapCounter& heap = counter();
  heap.trace = std::getenv("HEARTHMAP_HEAP_TRACE") != nullptr;
  CountingBulk bulk;
  ui::set_bulk_memory(&bulk);  // the embedder installs its port before it builds the UI
  {
    Scenario s = panel_like();
    HeadlessDisplay display(1280, 800);
    SizedPhotos photos;
    App app(display.screen(), s.config, s.readings, s.now, 0, &photos, kPanelPulseMemory, worker);
    app.screen().set_frames_deferred(true);
    Accepting presenter;
    std::uint32_t t = 0;
    // The panel's main loop, ms at a time: LVGL's timers, the frames they chose, a slice of
    // sprite preparation.
    auto run = [&](std::uint32_t ms) {
      for (const std::uint32_t end = t + ms; t < end; t += 16) {
        display.set_time_ms(t);
        display.run_timers();
        app.screen().show_due_frames(presenter);
        for (int i = 0; i < 40 && app.screen().prepare_step(); ++i) {
        }
        // A second thread works in real time: let it keep up with the simulated clock.
        for (int i = 0; worker != nullptr && i < 20000 && app.screen().prepare_step(); ++i) {
          std::this_thread::yield();
        }
      }
    };
    const CanvasRect& area = s.config.canvas.map_area;
    auto tap_pin = [&](std::size_t pin) {
      const MapPoint p = s.config.pins[pin].position;
      display.tap(static_cast<std::int32_t>(area.x + p.x * area.width),
                  static_cast<std::int32_t>(area.y + p.y * area.height));
      (void)app.process_events();
    };
    // Readings for round `k`: plants and rooms swing between fine and in danger, so pulses start
    // and stop and every heat image changes.
    auto readings_for = [&](int k) {
      Readings r = s.readings;
      int i = 0;
      for (auto& [entity, reading] : r.by_entity) {
        if (reading.state == ReadingState::Value && (i++ + k) % 3 == 0) {
          reading.value = reading.value < 10.0 ? 60.0 : (reading.value > 30.0 ? 21.0 : 33.0);
        }
      }
      return r;
    };
    const Layer layers[] = {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air};

    // Warm-up: every tab, the overlay once, some animation. What is sized once is sized now.
    for (Layer l : layers) {
      app.set_layer(l);
      run(500);
    }
    tap_pin(8);
    run(200);
    display.tap(20, 780);  // the scrim
    (void)app.process_events();
    run(200);

    const std::size_t baseline = heap.in_use.load();
    const std::size_t small_baseline = heap.small_in_use.load();
    heap.small_peak = small_baseline;
    const std::size_t bulk_baseline = bulk.in_use;
    heap.peak = baseline;
    heap.largest = 0;
    heap.over = 0;
    heap.over_bytes = 0;
    bulk.peak = bulk.in_use;
    heap.counting = true;
    std::size_t sprite_peak = 0;
    for (int round = 0; round < 3; ++round) {
      for (Layer l : layers) {
        app.set_layer(l);
        run(800);
        app.set_readings(readings_for(round * 4 + static_cast<int>(index_of(l))),
                         s.now + 60 * (round + 1));
        run(800);
        app.screen().invalidate_heat();  // every sprite recomposed over a new image
        run(400);
        sprite_peak = std::max(sprite_peak, app.screen().pulse_counters().tile_bytes);
      }
      tap_pin(8 + static_cast<std::size_t>(round));
      run(300);
      display.tap(20, 780);
      (void)app.process_events();
      run(300);
    }
    const std::size_t peak = heap.peak.load();
    const std::size_t small_peak = heap.small_peak.load();
    const std::size_t largest = heap.largest.load();
    const std::size_t over = heap.over.load();
    const std::size_t end = heap.in_use.load();
    // The probe's positive control: an allocation it must see, in the same counting state.
    std::size_t probe_largest = 0;
    std::size_t probe_over = 0;
    {
      std::vector<char> big(64U * 1024U);
      probe_largest = heap.largest.load();
      probe_over = heap.over.load() - over;
      big[0] = 1;
    }
    heap.counting = false;

    std::printf(
        "memory walk: default heap %zu B at the baseline, peak +%zu B, end %+lld B; largest "
        "single allocation %zu B; %zu allocations over %zu B (%zu B in all)\n",
        baseline, peak - baseline, static_cast<long long>(end) - static_cast<long long>(baseline),
        largest, over, kLargestAllowed, heap.over_bytes.load() - probe_largest);
    std::printf(
        "memory walk: in blocks of %zu B or less (internal RAM on the panel): %zu B at the "
        "baseline, peak +%zu B\n",
        kInternalClass, small_baseline, small_peak - small_baseline);
    std::printf(
        "memory walk: the port held %zu B at the baseline, peaked at %zu B over %zu allocations; "
        "sprites peaked at %zu B\n",
        bulk_baseline, bulk.peak, bulk.allocations, sprite_peak);

    CHECK(probe_largest >= 64U * 1024U);  // the counter sees what it must
    CHECK(probe_over == 1U);
    CHECK(peak - baseline <= kGrowthBudget);
    CHECK(small_peak - small_baseline <= kGrowthBudget);
    CHECK(over == 0U);
    CHECK(largest <= kLargestAllowed);
    CHECK(end <= baseline + kGrowthBudget);
    // The walk did the work the budget is about, and that work went through the port.
    CHECK(sprite_peak > 1024U * 1024U);
    CHECK(bulk.peak > bulk_baseline + 1024U * 1024U);
  }
  ui::set_bulk_memory(nullptr);
  CHECK(bulk.in_use == 0);  // everything taken from the port was given back to it
}

}  // namespace

TEST_CASE("memory: the UI's bulk storage stays off the default heap through a long walk") {
  long_walk(nullptr);
}

// The same with sprite tables and frames built on a second thread (D-069): its jobs allocate
// nothing (their caller sized every buffer through the port), so the budget is the same. The
// thread itself is made before the counted window, as the panel makes its task at boot.
TEST_CASE("memory: with sprites built on a worker thread, the default heap's budget still holds") {
  hearthmap::ui::ThreadWorker worker;
  long_walk(&worker);
}
