#include "diag_report.h"

#include <cstdio>

namespace esphome::hearthmap_panel {

std::uint32_t CounterAge::update(std::uint32_t now_ms, std::uint32_t value) {
  if (!seen_ || value != value_) {
    // The first sample starts the clock; every change restarts it.
    changed_ms_ = now_ms;
    seen_ = true;
    value_ = value;
    return 0;
  }
  return now_ms - changed_ms_;
}

namespace {
unsigned kib(std::size_t bytes) {
  return static_cast<unsigned>(bytes / 1024U);
}
unsigned secs(std::uint32_t ms) {
  return static_cast<unsigned>(ms / 1000U);
}
}  // namespace

int format_memory_line(std::uint32_t uptime_s, const MemorySample& m, char* out, std::size_t n) {
  return std::snprintf(
      out, n,
      "t=%us internal free %uK min %uK largest %uK | dma largest %uK (internal %uK) | "
      "psram free %uK largest %uK | failed allocs %u, dma %u (last %u B caps 0x%x)",
      static_cast<unsigned>(uptime_s), kib(m.internal_free), kib(m.internal_min),
      kib(m.internal_largest), kib(m.dma_largest), kib(m.dma_internal_largest), kib(m.psram_free),
      kib(m.psram_largest), static_cast<unsigned>(m.failed_allocs),
      static_cast<unsigned>(m.failed_dma_allocs), static_cast<unsigned>(m.last_failed_size),
      static_cast<unsigned>(m.last_failed_caps));
}

int format_link_line(const LinkSample& l, const char* rpc_err_name, char* out, std::size_t n) {
  char hosted[160];
  if (l.hosted_stats) {
    std::snprintf(hosted, sizeof(hosted),
                  "sdio tx in %u out %u drop %u stall %u (out %us ago) rx in %u (%us ago) "
                  "out %u (%us ago)",
                  static_cast<unsigned>(l.tx_in), static_cast<unsigned>(l.tx_out),
                  static_cast<unsigned>(l.tx_drop), static_cast<unsigned>(l.credit_stalls),
                  secs(l.tx_out_age_ms), static_cast<unsigned>(l.rx_in), secs(l.rx_in_age_ms),
                  static_cast<unsigned>(l.rx_out), secs(l.rx_out_age_ms));
  } else {
    std::snprintf(hosted, sizeof(hosted), "sdio counters not built");
  }
  char rpc[64];
  if (l.rpc_err == 0) {
    std::snprintf(rpc, sizeof(rpc), "rssi %d (%u ms)", l.rssi, static_cast<unsigned>(l.rpc_ms));
  } else {
    std::snprintf(rpc, sizeof(rpc), "rssi call %s (%u ms)",
                  rpc_err_name != nullptr ? rpc_err_name : "failed",
                  static_cast<unsigned>(l.rpc_ms));
  }
  char ping[64];
  if (l.ping_running) {
    std::snprintf(ping, sizeof(ping), "ping gw ok %u lost %u (reply %us ago)",
                  static_cast<unsigned>(l.ping_ok), static_cast<unsigned>(l.ping_lost),
                  secs(l.ping_age_ms));
  } else {
    std::snprintf(ping, sizeof(ping), "ping gw not started");
  }
  return std::snprintf(out, n, "%s | %s | %s | api %s | loop %u ms ago", hosted, rpc, ping,
                       l.api_connected ? "up" : "down", static_cast<unsigned>(l.loop_age_ms));
}

std::uint32_t share_permille(std::uint64_t task_delta, std::uint64_t total_delta) {
  if (total_delta == 0) return 0;
  return static_cast<std::uint32_t>(task_delta * 1000U / total_delta);
}

}  // namespace esphome::hearthmap_panel
