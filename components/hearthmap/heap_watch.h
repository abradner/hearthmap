// heap_watch.h — the panel's heap figures and failed allocations, for the internal-RAM guard's
// boot and perf lines (docs/decisions.md D-064) and the hosted-link diagnostics (D-063).
// Always built on the device: registering the failed-allocation hook and reading heap_caps'
// counters cost a few instructions per sample.
#pragma once

#include "diag_report.h"

namespace esphome::hearthmap_panel {

/// Registers ESP-IDF's failed-allocation hook (counting only). Once; later calls do nothing.
void start_heap_watch();
/// Internal, DMA-capable and PSRAM heap now, the lowest internal ever, and the hook's tally.
MemorySample sample_memory();

}  // namespace esphome::hearthmap_panel
