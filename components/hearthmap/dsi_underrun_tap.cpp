#include "dsi_underrun_tap.h"

#include <atomic>

#include "phrase_matcher.h"
#include "sdkconfig.h"

#if defined(CONFIG_SOC_MIPI_DSI_SUPPORTED) && defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#define HEARTHMAP_DSI_TAP 1
#include "esp_attr.h"
#include "esp_rom_sys.h"
#endif

namespace esphome::hearthmap_panel {

namespace {

std::atomic<std::uint32_t> g_underruns{0};

#ifdef HEARTHMAP_DSI_TAP
// The tail of the driver's message (ESP-IDF 5.5.6 esp_lcd_panel_dpi.c). In RAM, not flash: the
// tap can run in an interrupt handler while the flash cache is off.
PhraseMatcher<16> g_phrase("underrun happens");
std::atomic<std::uint32_t> g_chars{0};

// Every character esp_rom_printf writes, from any task or interrupt handler. Characters from two
// cores printing at once interleave, which can lose a count; nothing here can block.
void IRAM_ATTR on_console_char(char c) {
  g_chars.fetch_add(1, std::memory_order_relaxed);
  if (g_phrase.feed(c)) g_underruns.fetch_add(1, std::memory_order_relaxed);
}
#endif

}  // namespace

bool start_dsi_underrun_tap() {
#ifdef HEARTHMAP_DSI_TAP
  static bool live = false;
  static bool tried = false;
  if (tried) return live;
  tried = true;
  esp_rom_install_channel_putc(2, on_console_char);
  // Proves the channel reaches the tap: this line goes to the console through it.
  const std::uint32_t before = g_chars.load(std::memory_order_relaxed);
  esp_rom_printf("[I][hearthmap]: counting DSI underruns from the panel driver's console line\n");
  live = g_chars.load(std::memory_order_relaxed) != before;
  if (!live) esp_rom_install_channel_putc(2, nullptr);
  return live;
#else
  return false;
#endif
}

std::uint32_t dsi_underruns() {
  return g_underruns.load(std::memory_order_relaxed);
}

}  // namespace esphome::hearthmap_panel
