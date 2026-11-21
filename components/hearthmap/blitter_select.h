// blitter_select.h — builds the Blitter the YAML option `blitter:` names (docs/decisions.md D-057).
#pragma once

#include <memory>

#include "hearthmap/ui/blitter.h"

namespace esphome::hearthmap_panel {

enum class BlitterKind : uint8_t { Software, Ppa };

/// The Blitter for `kind`, and in `*name` what was actually built. `Ppa` on a target without a
/// PPA, or when the PPA driver refuses its clients, gives the SoftwareBlitter: a target without
/// the accelerator loses only speed.
std::unique_ptr<::hearthmap::ui::Blitter> make_blitter(BlitterKind kind, const char** name);

}  // namespace esphome::hearthmap_panel
