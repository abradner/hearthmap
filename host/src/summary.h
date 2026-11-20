// summary.h — the top bar's house summary and clock text, formatted on the host.
//
// Stopgap (docs/decisions.md D-030): the house summary is an average over core's assessments,
// computed here because core has no summary function yet. The averaging is tested in
// host/tests; it belongs in core once a view-model field for it exists.
#pragma once

#include <string>

#include "hearthmap/config.h"
#include "hearthmap/readings.h"

namespace hearthmap::host {

/// "House 21.4°   52% RH   PM2.5 6": the mean of fresh temperature and humidity readings that
/// feed their layers on indoor pins (core's assess() and feeds_layer()), and the mean fresh
/// indoor PM2.5. A part with no fresh reading is left out; with none at all, "House: no data".
std::string house_summary(const Config& config, const Readings& readings, Timestamp now);

/// "HH:MM" for `now` shifted by `utc_offset_minutes` (the panel's time zone is the
/// embedder's business).
std::string clock_text(Timestamp now, int utc_offset_minutes);

}  // namespace hearthmap::host
