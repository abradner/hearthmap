// readings_ingest.h — Home Assistant state strings -> core Readings, and the freshness rule.
//
// Pure C++17 over core's readings.h: no ESPHome, no LVGL, no clock, so it is compiled and tested
// on a laptop (components/hearthmap/tests/test_cpp.py). The ESPHome component calls it from
// its API callbacks and its refresh timer.
#pragma once

#include <cstddef>
#include <string_view>

#include "hearthmap/readings.h"

namespace hearthmap_device {

/// One HA state string as a Reading, with last_updated = `now`.
/// - "unavailable" -> Unavailable; "unknown" -> Unknown (HA's own spellings, exact match).
/// - A decimal number (optional sign, digits, optional fraction, optional exponent, surrounding
///   ASCII spaces allowed) that is finite -> Value. Locale-independent: '.' is the only decimal
///   point.
/// - Anything else, including an empty string, "nan", "inf" and a number with trailing text
///   ("21.5 C"), -> Unknown (readings.h: sources map non-finite numbers to Unknown).
hearthmap::Reading parse_state(std::string_view state, hearthmap::Timestamp now);

/// Record a state update for `entity_id`: replaces state, value and last_updated, keeps the
/// unit already known for the entity (units arrive on their own subscription).
void apply_state(hearthmap::Readings& readings, std::string_view entity_id, std::string_view state,
                 hearthmap::Timestamp now);

/// Record the unit attribute for `entity_id`. Creates an entry in state Unknown if the entity
/// has no state yet, with last_updated 0: a unit is not a reading.
void apply_unit(hearthmap::Readings& readings, std::string_view entity_id, std::string_view unit);

/// Ruling R8 (docs/decisions.md D-012): last_updated means "last confirmed". Home Assistant pushes
/// only on change, so while the API connection with a state subscription is up every reading in
/// state Value is re-stamped to `now`. While it is down nothing is touched, so readings age
/// honestly into TooOld. Unavailable and Unknown readings are never re-stamped: HA confirming
/// "unavailable" says nothing new about a value. Returns how many readings were re-stamped.
std::size_t restamp(hearthmap::Readings& readings, bool api_connected, hearthmap::Timestamp now);

}  // namespace hearthmap_device
