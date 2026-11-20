// readings.h — sensor readings as core receives them: data in, no I/O.
//
// The ESPHome component fills a Readings from Home Assistant state updates; the host fills one
// from a fixture file (examples/README.md, "Readings fixtures"). Core only reads it.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "hearthmap/config.h"

namespace hearthmap {

/// What Home Assistant said about an entity, reduced to what core needs.
enum class ReadingState : std::uint8_t {
  Value,        ///< A numeric state; `value` holds it.
  Unavailable,  ///< HA reported "unavailable".
  Unknown,      ///< HA reported "unknown", or a state that is not a number.
};

struct Reading {
  ReadingState state = ReadingState::Unknown;
  double value = 0.0;  ///< Meaningful only when state == Value. Never NaN or inf there:
                       ///< sources map non-finite numbers to Unknown.
  std::string unit;    ///< As HA reports it ("°C", "%"). Informational: core does not
                       ///< convert units (docs/decisions.md D-011).
  /// The last time the source confirmed this value: not the last time it changed
  /// (docs/decisions.md D-012). Home Assistant pushes a state only when it changes, so a steady
  /// sensor would otherwise age into TooOld while still reporting. The device glue therefore
  /// re-stamps every reading in state Value with the current time while the Home Assistant API
  /// connection is up, and stops re-stamping the moment it drops, so readings then age
  /// honestly. Fixtures and the host pass timestamps through exactly as given.
  Timestamp last_updated = 0;
};

/// All readings known at one moment, keyed by entity id. An entity absent from the map is
/// "missing", which core treats exactly like a reading it has never received.
struct Readings {
  std::map<std::string, Reading, std::less<>> by_entity;

  /// The reading for `entity_id`, or nullptr when missing.
  const Reading* find(std::string_view entity_id) const {
    auto it = by_entity.find(entity_id);
    return it == by_entity.end() ? nullptr : &it->second;
  }
};

}  // namespace hearthmap
