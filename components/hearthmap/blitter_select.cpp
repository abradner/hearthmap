#include "blitter_select.h"

#include "ppa_blitter.h"

namespace esphome::hearthmap_panel {

std::unique_ptr<::hearthmap::ui::Blitter> make_blitter(BlitterKind kind, const char** name) {
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (kind == BlitterKind::Ppa) {
    auto ppa = std::make_unique<PpaBlitter>();
    if (ppa->begin()) {
      *name = "ppa";
      return ppa;
    }
    *name = "software (PPA clients refused)";
    return std::make_unique<::hearthmap::ui::SoftwareBlitter>();
  }
#else
  if (kind == BlitterKind::Ppa) {
    *name = "software (no PPA on this target)";
    return std::make_unique<::hearthmap::ui::SoftwareBlitter>();
  }
#endif
  *name = "software";
  return std::make_unique<::hearthmap::ui::SoftwareBlitter>();
}

}  // namespace esphome::hearthmap_panel
