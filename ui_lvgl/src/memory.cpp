#include "hearthmap/ui/memory.h"

#include <atomic>
#include <cstdlib>

namespace hearthmap::ui {

namespace {

class SystemBulkMemory final : public BulkMemory {
 public:
  void* allocate(std::size_t bytes, std::size_t align) override {
    // malloc's alignment covers every type the UI stores; nothing asks for more.
    if (bytes == 0 || align > alignof(std::max_align_t)) return nullptr;
    return std::malloc(bytes);  // NOLINT: this is the system heap's port
  }
  void deallocate(void* p, std::size_t /*bytes*/) override {
    std::free(p);  // NOLINT
  }
};

SystemBulkMemory g_system;
BulkMemory* g_memory = &g_system;
std::atomic<std::size_t> g_in_use{0};
std::atomic<std::size_t> g_peak{0};
std::atomic<std::uint32_t> g_failures{0};
std::atomic<std::size_t> g_last_failed{0};
std::atomic<BulkFailureHandler> g_handler{nullptr};

}  // namespace

BulkMemory& system_bulk_memory() {
  return g_system;
}

BulkMemory& bulk_memory() {
  return *g_memory;
}

bool set_bulk_memory(BulkMemory* memory) {
  if (g_in_use.load(std::memory_order_relaxed) != 0) return false;
  g_memory = memory != nullptr ? memory : &g_system;
  g_peak.store(0, std::memory_order_relaxed);
  g_failures.store(0, std::memory_order_relaxed);
  g_last_failed.store(0, std::memory_order_relaxed);
  return true;
}

BulkStats bulk_stats() {
  BulkStats s;
  s.in_use = g_in_use.load(std::memory_order_relaxed);
  s.peak = g_peak.load(std::memory_order_relaxed);
  s.failures = g_failures.load(std::memory_order_relaxed);
  s.last_failed_bytes = g_last_failed.load(std::memory_order_relaxed);
  return s;
}

void* bulk_allocate(std::size_t bytes, std::size_t align) {
  if (bytes == 0) bytes = 1;  // a distinct pointer, as `new` gives for zero
  void* p = g_memory->allocate(bytes, align);
  if (p == nullptr) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    g_last_failed.store(bytes, std::memory_order_relaxed);
    return nullptr;
  }
  const std::size_t now = g_in_use.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  std::size_t peak = g_peak.load(std::memory_order_relaxed);
  while (now > peak && !g_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
  return p;
}

void bulk_free(void* p, std::size_t bytes) {
  if (p == nullptr) return;
  if (bytes == 0) bytes = 1;
  g_memory->deallocate(p, bytes);
  g_in_use.fetch_sub(bytes, std::memory_order_relaxed);
}

void set_bulk_failure_handler(BulkFailureHandler handler) {
  g_handler.store(handler, std::memory_order_relaxed);
}

void bulk_exhausted(std::size_t bytes) {
  const BulkFailureHandler h = g_handler.load(std::memory_order_relaxed);
  if (h != nullptr) h(bytes);
  std::abort();
}

}  // namespace hearthmap::ui
