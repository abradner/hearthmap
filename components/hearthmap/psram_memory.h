// psram_memory.h — the panel's memory port for the UI's bulk storage: PSRAM only
// (docs/decisions.md D-064; the port is hearthmap/ui/memory.h).
//
// Every buffer ui_lvgl sizes by pixels, cells or pulses, and the component's heat images, come
// from here. A request PSRAM cannot meet is refused (and counted by the port), never retried in
// internal RAM: internal RAM is where the hosted Wi-Fi driver needs its DMA frames, and filling
// it with UI buffers is the fault this port exists to prevent. On a target without PSRAM every
// request is refused; such a target needs a port of its own.
#pragma once

#include <cstddef>

#include "hearthmap/ui/memory.h"

namespace esphome::hearthmap_panel {

class PsramMemory final : public ::hearthmap::ui::BulkMemory {
 public:
  void* allocate(std::size_t bytes, std::size_t align) override;
  void deallocate(void* p, std::size_t bytes) override;
};

/// The port the component installs in setup() (one per process, like LVGL).
PsramMemory& psram_memory();

/// For the port's failure handler: logs the request that could not be met, straight to the UART
/// (the main loop may be about to stop).
void report_bulk_exhausted(std::size_t bytes);

}  // namespace esphome::hearthmap_panel
