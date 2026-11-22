// stall_monitor.h — a small task that names the main loop's wait when it stops turning
// (docs/decisions.md D-062). Logs at WARN once the loop has not turned for about 2 s, at ERROR
// half a second before the task watchdog would reset the chip, and at WARN again when the loop
// comes back; each line names the breadcrumb (loop_breadcrumb.h) and how long it has been held.
// Lines go straight to the UART console (esp_rom_printf), since ESPHome prints other tasks' log
// lines from the main loop, which is the thing that is stuck.
#pragma once

#include <cstdint>

namespace esphome::hearthmap_panel {

/// Starts the monitor task once. `watchdog_ms`: the task watchdog's timeout (esp32:
/// watchdog_timeout). Returns false if the task could not be created.
bool start_stall_monitor(std::uint32_t watchdog_ms);

}  // namespace esphome::hearthmap_panel
