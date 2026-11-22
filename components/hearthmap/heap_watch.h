// heap_watch.h — the panel's heap figures and failed allocations, for the internal-RAM guard's
// boot and perf lines (docs/decisions.md D-064) and the hosted-link diagnostics (D-063).
// Always built on the device: registering the failed-allocation hook costs nothing until an
// allocation fails.
//
// Two kinds of heap figure, and they are not alike (D-067). Free bytes and the lowest-ever free
// are counters each heap keeps: reading them is O(1) and takes no lock. The largest free block
// is found by walking every block of every matching heap (heap_caps_get_info) inside that heap's
// spinlock, which on this core is a critical section: interrupts off for the whole walk, and on
// the 26 MB PSRAM heap that is a walk of thousands of headers through the cache. The DPI panel
// re-arms its frame DMA from an interrupt at the end of every frame; held off long enough, the
// panel's FIFO runs dry and the frame shows the DSI bridge's underrun colour, a uniform blue.
// So anything periodic reads counters only, and a walk is for boot, for a warning that is
// already rare, or for `heap_probe: walk`, which brings the old once-a-second walks back to
// reproduce the flash.
#pragma once

#include <cstdint>

#include "diag_report.h"

namespace esphome::hearthmap_panel {

/// What the periodic memory check reads (`heap_probe` in YAML).
enum class HeapProbe : std::uint8_t {
  Off,       ///< nothing: the internal-RAM guard is blind, the perf mem line keeps boot figures
  Counters,  ///< free and lowest-free only: O(1), no lock (the default)
  Walk,      ///< counters plus every largest-free-block walk, once a second (before D-067)
};

/// Registers ESP-IDF's failed-allocation hook (counting only). Once; later calls do nothing.
void start_heap_watch();
/// Counters only (free, lowest free, PSRAM free, the hook's tally): O(1), no heap lock. The
/// largest-block fields are 0 and `walked` is false.
MemorySample sample_memory_counters();
/// Counters plus every largest-free-block walk (interrupts off on this core while each heap is
/// walked). Not for anything periodic; see the header comment.
MemorySample sample_memory();
/// The longest sample_memory*() call since the last take, in microseconds (0 if none), then
/// starts again: the perf line's "heap probe max", an upper bound on the interrupts-off time.
std::uint32_t take_heap_probe_max_us();

}  // namespace esphome::hearthmap_panel
