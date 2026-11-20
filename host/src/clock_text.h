// clock_text.h — the top bar's clock text, formatted on the host.
//
// The house summary used to live here too, as a stopgap (docs/decisions.md D-030); it is now
// core's house_summary() and ui_lvgl's summary_text() (D-049), shared with the device.
#pragma once

#include <string>

#include "hearthmap/config.h"

namespace hearthmap::host {

/// "HH:MM" for `now` shifted by `utc_offset_minutes` (the panel's time zone is the
/// embedder's business).
std::string clock_text(Timestamp now, int utc_offset_minutes);

}  // namespace hearthmap::host
