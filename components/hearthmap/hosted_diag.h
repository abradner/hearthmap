// hosted_diag.h — diagnostics for the hosted Wi-Fi link (docs/decisions.md D-063), built only
// with `hearthmap: diagnostics: true` (HEARTHMAP_DIAGNOSTICS). A small task prints, every 10 s,
// straight to the UART console (esp_rom_printf, like the stall monitor, so the lines arrive even
// when the main loop is blocked): internal, DMA-capable and PSRAM heap, every failed allocation,
// esp-hosted's SDIO packet counters and how long each has stood still, one timed Wi-Fi call
// (RSSI) to the co-processor, a ping to the gateway through lwIP, the API connection and the main
// loop's age; every 60 s, each task's CPU share and stack headroom. Without the option every
// function here is an empty inline.
#pragma once

namespace esphome::hearthmap_panel {

#ifdef HEARTHMAP_DIAGNOSTICS
/// Registers the failed-allocation hook, opens esp-hosted's and ESP-IDF's warnings, and starts
/// the report task. Once; later calls do nothing.
void start_hosted_diagnostics();
/// From the main loop: whether Home Assistant's API connection is up, for the report.
void diag_note_api(bool connected);
#else
inline void start_hosted_diagnostics() {}
inline void diag_note_api(bool /*connected*/) {}
#endif

}  // namespace esphome::hearthmap_panel
