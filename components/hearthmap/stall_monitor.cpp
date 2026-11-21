#include "stall_monitor.h"

#include "esp_rom_sys.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loop_breadcrumb.h"

namespace esphome::hearthmap_panel {

static const char* const TAG = "hearthmap.stall";

namespace {
constexpr std::uint32_t kWarnMs = 2000;
constexpr std::uint32_t kPeriodMs = 250;

struct Args {
  std::uint32_t error_ms;
};

void monitor_task(void* arg) {
  const std::uint32_t error_ms = static_cast<Args*>(arg)->error_ms;
  StallJudge judge(kWarnMs, error_ms);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    LoopBreadcrumb& b = breadcrumb();
    // The heartbeat first, then the clock: the other way round, a loop turn between the two
    // reads would look like a heartbeat from the future.
    const std::uint32_t turn = b.turn_ms.load(std::memory_order_relaxed);
    const std::uint32_t now = millis();
    std::uint32_t stalled = 0;
    const StallJudge::Verdict v = judge.step(now, turn, &stalled);
    if (v == StallJudge::Verdict::None) continue;
    const auto crumb = static_cast<Crumb>(b.crumb.load(std::memory_order_relaxed));
    const std::uint32_t held = now - b.since_ms.load(std::memory_order_relaxed);
    // Written straight to the UART console: ESPHome queues another task's log lines for the main
    // loop to print, and the main loop is the thing that is stuck. The ESP_LOG line follows for
    // API log subscribers, and appears once the loop turns again.
    if (v == StallJudge::Verdict::Warn) {
      esp_rom_printf("[W][%s]: main loop has not turned for %u ms; in: %s (for %u ms)\n", TAG,
                     (unsigned)stalled, crumb_name(crumb), (unsigned)held);
      ESP_LOGW(TAG, "main loop did not turn for %u ms; in: %s (for %u ms)", (unsigned)stalled,
               crumb_name(crumb), (unsigned)held);
    } else if (v == StallJudge::Verdict::Error) {
      esp_rom_printf("[E][%s]: main loop stuck %u ms, task watchdog due; in: %s (for %u ms)\n", TAG,
                     (unsigned)stalled, crumb_name(crumb), (unsigned)held);
    } else {
      esp_rom_printf("[W][%s]: main loop turned again after a stall of at least %u ms\n", TAG,
                     (unsigned)stalled);
      ESP_LOGW(TAG, "main loop turned again after a stall of at least %u ms", (unsigned)stalled);
    }
  }
}
}  // namespace

bool start_stall_monitor(std::uint32_t watchdog_ms) {
  static bool started = false;
  static Args args;
  if (started) return true;
  args.error_ms = watchdog_ms > 1000 ? watchdog_ms - 500 : watchdog_ms;
  // Low priority, but above idle; it never blocks anything and logs only on a stall.
  started = xTaskCreate(monitor_task, "hm_stall", 3072, &args, 2, nullptr) == pdPASS;
  return started;
}

}  // namespace esphome::hearthmap_panel
