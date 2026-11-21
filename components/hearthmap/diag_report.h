// diag_report.h — the pure parts of the hosted Wi-Fi link diagnostics (hosted_diag.h,
// docs/decisions.md D-063): how long a counter has stood still, a task's CPU share, and the text
// of the report lines. No ESPHome, no FreeRTOS, no heap: host-tested in tests/test_cpp.py.
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::hearthmap_panel {

/// Milliseconds since a counter last changed, on a wrapping millisecond clock.
class CounterAge {
 public:
  /// Takes `value` at `now_ms` and returns how long it has held that value. The first sample
  /// starts the clock: a counter never seen to move reports the time since then.
  std::uint32_t update(std::uint32_t now_ms, std::uint32_t value);

 private:
  bool seen_ = false;
  std::uint32_t value_ = 0;
  std::uint32_t changed_ms_ = 0;
};

/// Heap figures, bytes, plus the failed-allocation hook's tally.
struct MemorySample {
  std::size_t internal_free = 0;
  std::size_t internal_min = 0;  ///< the lowest internal free ever seen (heap_caps)
  std::size_t internal_largest = 0;
  std::size_t dma_internal_largest = 0;  ///< MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
  std::size_t dma_largest = 0;           ///< MALLOC_CAP_DMA alone: what esp-hosted's packets ask
  std::size_t psram_free = 0;
  std::size_t psram_largest = 0;
  std::uint32_t failed_allocs = 0;      ///< allocations that returned NULL, since boot
  std::uint32_t failed_dma_allocs = 0;  ///< of those, ones that asked for DMA-capable memory
  std::size_t last_failed_size = 0;
  std::uint32_t last_failed_caps = 0;
};

/// The co-processor link as seen from the P4.
struct LinkSample {
  bool hosted_stats = false;  ///< false: the build has no esp-hosted packet counters
  std::uint32_t tx_in = 0, tx_out = 0, tx_drop = 0, credit_stalls = 0;
  std::uint32_t rx_in = 0, rx_out = 0;
  std::uint32_t rx_in_age_ms = 0;   ///< since a frame was last read from the co-processor
  std::uint32_t rx_out_age_ms = 0;  ///< since a frame was last handed to the network stack
  std::uint32_t tx_out_age_ms = 0;  ///< since a frame was last written to the co-processor
  int rpc_err = 0;                  ///< 0: the Wi-Fi call answered; else its esp_err_t
  int rssi = 0;                     ///< valid when rpc_err is 0
  std::uint32_t rpc_ms = 0;         ///< how long the call took
  bool ping_running = false;        ///< false: no gateway known yet
  std::uint32_t ping_ok = 0, ping_lost = 0;
  std::uint32_t ping_age_ms = 0;  ///< since the last reply (or since pinging began)
  bool api_connected = false;
  std::uint32_t loop_age_ms = 0;  ///< since hearthmap's loop() last started
};

/// One line of heap figures, in KiB (internal, DMA, PSRAM, failed allocations).
int format_memory_line(std::uint32_t uptime_s, const MemorySample& m, char* out, std::size_t n);
/// One line of link figures. `rpc_err_name` names rpc_err (esp_err_to_name on the panel).
int format_link_line(const LinkSample& l, const char* rpc_err_name, char* out, std::size_t n);
/// A task's share of one core over a window, in tenths of a percent (0 when the window is empty).
std::uint32_t share_permille(std::uint64_t task_delta, std::uint64_t total_delta);

}  // namespace esphome::hearthmap_panel
