#include "hosted_diag.h"

#ifdef HEARTHMAP_DIAGNOSTICS

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "diag_report.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esphome/core/hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_watch.h"
#include "loop_breadcrumb.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_HOSTED_HOST_PKT_STATS
// esp-hosted 3.1.0's host datapath counters (host/mcu/eh_host_mcu_transport/include/
// eh_host_pkt_stats.h, `struct pkt_stats_t pkt_stats`), incremented by its SDIO driver. Only the
// leading fields, which every build has (USB RNDIS counters, when built, come after them); that
// header is not on this component's include path. Re-check the layout on an esp-hosted bump.
extern "C" {
struct HmHostedPktStats {
  uint32_t sta_tx_in_pass;    // accepted by bus_tx
  uint32_t sta_tx_trans_in;   // entered the bus write path
  uint32_t sta_tx_out;        // sent to the co-processor
  uint32_t sta_tx_out_drop;   // dropped in the write path (no DMA buffer)
  uint32_t sta_rx_in;         // frames read from the co-processor
  uint32_t sta_rx_out;        // delivered to the network stack
  uint32_t tx_credit_stalls;  // aggregates that timed out waiting for co-processor credit
};
extern HmHostedPktStats pkt_stats;
}
#endif

namespace esphome::hearthmap_panel {

namespace {

constexpr std::uint32_t kPeriodMs = 10000;
constexpr std::uint32_t kTaskEvery = 6;  // the task table every 60 s
constexpr const char* kPrefix = "[I][hm.diag]: ";

std::atomic<bool> g_api{false};

std::atomic<std::uint32_t> g_ping_ok{0};
std::atomic<std::uint32_t> g_ping_lost{0};
std::atomic<std::uint32_t> g_ping_reply_ms{0};
esp_ping_handle_t g_ping = nullptr;
std::uint32_t g_ping_started_ms = 0;

void put(const char* line) {
  esp_rom_printf("%s%s\n", kPrefix, line);
}

void on_ping_success(esp_ping_handle_t /*h*/, void* /*arg*/) {
  g_ping_ok.fetch_add(1, std::memory_order_relaxed);
  g_ping_reply_ms.store(millis(), std::memory_order_relaxed);
}

void on_ping_timeout(esp_ping_handle_t /*h*/, void* /*arg*/) {
  g_ping_lost.fetch_add(1, std::memory_order_relaxed);
}

// One ICMP echo to the gateway every period, from lwIP's own ping task, once an address exists.
void start_ping_once() {
  if (g_ping != nullptr) return;
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t ip{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) return;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  ip_addr_set_ip4_u32(&cfg.target_addr, ip.gw.addr);
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = kPeriodMs;
  cfg.timeout_ms = 2000;
  esp_ping_callbacks_t cbs{};
  cbs.on_ping_success = on_ping_success;
  cbs.on_ping_timeout = on_ping_timeout;
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) return;
  if (esp_ping_start(h) != ESP_OK) {
    esp_ping_delete_session(h);
    return;
  }
  g_ping = h;
  g_ping_started_ms = millis();
}

#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
// Each task's CPU share of one core since the last table, and its stack headroom (bytes: an
// ESP-IDF stack word is a byte).
void print_tasks() {
  constexpr UBaseType_t kMax = 48;
  struct Prev {
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE run;
  };
  static TaskStatus_t* tasks = nullptr;
  static Prev prev[kMax];
  static UBaseType_t prev_n = 0;
  static configRUN_TIME_COUNTER_TYPE prev_total = 0;
  if (tasks == nullptr) {
    tasks = static_cast<TaskStatus_t*>(
        heap_caps_malloc(kMax * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tasks == nullptr) return;
  }
  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t n = uxTaskGetSystemState(tasks, kMax, &total);
  if (n == 0) {
    put("tasks: more than 48, table skipped");
    return;
  }
  char line[128];
  for (UBaseType_t i = 0; i < n; ++i) {
    const TaskStatus_t& t = tasks[i];
    configRUN_TIME_COUNTER_TYPE before = 0;
    bool known = false;
    for (UBaseType_t k = 0; k < prev_n; ++k) {
      if (prev[k].handle == t.xHandle) {
        before = prev[k].run;
        known = true;
        break;
      }
    }
    const std::uint32_t share =
        known ? share_permille(t.ulRunTimeCounter - before, total - prev_total) : 0U;
    std::snprintf(line, sizeof(line), "task %-16s prio %2u cpu %3u.%u%% stack free %u B%s",
                  t.pcTaskName, static_cast<unsigned>(t.uxCurrentPriority),
                  static_cast<unsigned>(share / 10U), static_cast<unsigned>(share % 10U),
                  static_cast<unsigned>(t.usStackHighWaterMark), known ? "" : " (new)");
    put(line);
  }
  for (UBaseType_t i = 0; i < n; ++i) prev[i] = Prev{tasks[i].xHandle, tasks[i].ulRunTimeCounter};
  prev_n = n;
  prev_total = total;
}
#else
void print_tasks() {}
#endif

void diag_task(void* /*arg*/) {
  CounterAge rx_in_age;
  CounterAge rx_out_age;
  CounterAge tx_out_age;
  TickType_t wake = xTaskGetTickCount();
  char line[384];
  for (std::uint32_t round = 1;; ++round) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(kPeriodMs));
    const std::uint32_t now = millis();
    format_memory_line(now / 1000U, sample_memory(), line, sizeof(line));
    put(line);

    LinkSample l;
#ifdef CONFIG_ESP_HOSTED_HOST_PKT_STATS
    l.hosted_stats = true;
    l.tx_in = pkt_stats.sta_tx_in_pass;
    l.tx_out = pkt_stats.sta_tx_out;
    l.tx_drop = pkt_stats.sta_tx_out_drop;
    l.credit_stalls = pkt_stats.tx_credit_stalls;
    l.rx_in = pkt_stats.sta_rx_in;
    l.rx_out = pkt_stats.sta_rx_out;
    l.rx_in_age_ms = rx_in_age.update(now, l.rx_in);
    l.rx_out_age_ms = rx_out_age.update(now, l.rx_out);
    l.tx_out_age_ms = tx_out_age.update(now, l.tx_out);
#endif
    l.api_connected = g_api.load(std::memory_order_relaxed);
    start_ping_once();
    l.ping_running = g_ping != nullptr;
    l.ping_ok = g_ping_ok.load(std::memory_order_relaxed);
    l.ping_lost = g_ping_lost.load(std::memory_order_relaxed);
    // Stamps other tasks write, read before the clock they are aged against (`now` above is
    // older than the memory line's printing; elapsed_ms() also clamps a stamp from after it).
    const std::uint32_t turn = breadcrumb().turn_ms.load(std::memory_order_relaxed);
    const std::uint32_t reply = g_ping_reply_ms.load(std::memory_order_relaxed);
    const std::uint32_t ping_from = reply != 0 ? reply : g_ping_started_ms;
    const std::uint32_t aged_at = millis();
    l.loop_age_ms = elapsed_ms(aged_at, turn);
    l.ping_age_ms = elapsed_ms(aged_at, ping_from);
    // A remote call to the co-processor, from this task: it may wait the whole RPC timeout, which
    // blocks only this report.
    wifi_ap_record_t ap{};
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    l.rpc_ms = static_cast<std::uint32_t>((esp_timer_get_time() - t0) / 1000);
    l.rpc_err = err;
    l.rssi = ap.rssi;
    format_link_line(l, esp_err_to_name(err), line, sizeof(line));
    put(line);
    if (round % kTaskEvery == 0) print_tasks();
  }
}

}  // namespace

void diag_note_api(bool connected) {
  g_api.store(connected, std::memory_order_relaxed);
}

void start_hosted_diagnostics() {
  static bool started = false;
  if (started) return;
  started = true;
  start_heap_watch();  // the failed-allocation tally (also started by the component itself)
  // ESPHome builds ESP-IDF's log at ERROR unless `esp32: framework: log_level` says otherwise,
  // which hides esp-hosted's "mempool OOM start", "dma_alloc failed" and "rx_get_buffer failed"
  // (all WARN). The component raises the compiled ceiling to INFO in this mode; WARN is enough
  // here, and quieter than INFO.
  esp_log_level_set("*", ESP_LOG_WARN);
  // Above the main loop (1) so the report runs while the loop is busy; it sleeps 10 s at a time.
  if (xTaskCreate(diag_task, "hm_diag", 6144, nullptr, 3, nullptr) != pdPASS) {
    esp_rom_printf("%sreport task not started (no memory)\n", kPrefix);
  }
}

}  // namespace esphome::hearthmap_panel

#endif  // HEARTHMAP_DIAGNOSTICS
