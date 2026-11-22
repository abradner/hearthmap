// dsi_underrun_tap.h — counts the MIPI-DSI panel's FIFO underruns (docs/decisions.md D-067).
//
// When the DPI panel's frame DMA cannot keep the DSI bridge's FIFO fed, the bridge sends its
// reserved pixel value for the rest of the frame, which on this panel is a uniform blue, and
// ESP-IDF's DPI driver (esp_lcd_panel_dpi.c, mipi_dsi_bridge_isr_handler, 5.5.6) says only
// "can't fetch data from external memory fast enough, underrun happens", from its interrupt
// handler, straight to the ROM console with esp_rom_printf. That bypasses ESP-IDF's log hook,
// so it never reaches ESPHome's log, and the driver exposes no counter or callback for it.
//
// So the tap listens on the ROM console's second output channel (unused unless a secondary
// console is configured, and then the tap stays off) and counts that phrase. The perf mem line
// reports the count per window; "not counted" means the tap could not be installed.
#pragma once

#include <cstdint>

namespace esphome::hearthmap_panel {

/// Installs the tap once and checks that it sees console output; false if the build cannot
/// have it (no MIPI-DSI, or a secondary console owns the channel) or the check saw nothing.
bool start_dsi_underrun_tap();
/// Underruns counted since start-up (0 if the tap is off).
std::uint32_t dsi_underruns();

}  // namespace esphome::hearthmap_panel
