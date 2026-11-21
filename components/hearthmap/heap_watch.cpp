#include "heap_watch.h"

#include <atomic>
#include <cstdint>

#include "esp_heap_caps.h"

namespace esphome::hearthmap_panel {

namespace {

std::atomic<std::uint32_t> g_failed{0};
std::atomic<std::uint32_t> g_failed_dma{0};
std::atomic<std::uint32_t> g_last_failed_size{0};
std::atomic<std::uint32_t> g_last_failed_caps{0};

// Runs in the allocating task, perhaps a driver's: counts only.
void on_alloc_failed(size_t size, uint32_t caps, const char* /*function_name*/) {
  g_failed.fetch_add(1, std::memory_order_relaxed);
  if ((caps & MALLOC_CAP_DMA) != 0) g_failed_dma.fetch_add(1, std::memory_order_relaxed);
  g_last_failed_size.store(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
  g_last_failed_caps.store(caps, std::memory_order_relaxed);
}

}  // namespace

void start_heap_watch() {
  static bool started = false;
  if (started) return;
  started = true;
  heap_caps_register_failed_alloc_callback(on_alloc_failed);
}

MemorySample sample_memory() {
  MemorySample m;
  m.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  m.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  m.dma_internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
  m.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  m.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  m.failed_allocs = g_failed.load(std::memory_order_relaxed);
  m.failed_dma_allocs = g_failed_dma.load(std::memory_order_relaxed);
  m.last_failed_size = g_last_failed_size.load(std::memory_order_relaxed);
  m.last_failed_caps = g_last_failed_caps.load(std::memory_order_relaxed);
  return m;
}

}  // namespace esphome::hearthmap_panel
