#include "psram_memory.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

namespace esphome::hearthmap_panel {

void* PsramMemory::allocate(std::size_t bytes, std::size_t align) {
  // PSRAM and nothing else: no MALLOC_CAP_INTERNAL fallback, unlike ESPHome's RAMAllocator.
  return heap_caps_aligned_alloc(std::max<std::size_t>(align, sizeof(void*)), bytes,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void PsramMemory::deallocate(void* p, std::size_t /*bytes*/) {
  heap_caps_free(p);  // frees heap_caps_aligned_alloc's blocks too (ESP-IDF 5.x)
}

PsramMemory& psram_memory() {
  static PsramMemory memory;
  return memory;
}

void report_bulk_exhausted(std::size_t bytes) {
  esp_rom_printf(
      "[E][hearthmap]: PSRAM refused %u bytes of UI storage (largest free %u); "
      "stopping rather than taking internal RAM (D-064)\n",
      static_cast<unsigned>(bytes),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

}  // namespace esphome::hearthmap_panel
