// flat_fixture.h — examples/flat/ rebuilt in C++ for core's tests (core has no JSON parser).
//
// Mirrors examples/flat/config.json and its readings fixtures field for field, with the
// schema defaults filled in the way a ConfigSource would (docs/decisions.md D-007). If the
// example changes, change this too; the host renderer's golden images are the check
// that the JSON and this copy agree.
#pragma once

#include <cstddef>
#include <string>

#include "hearthmap/config.h"
#include "hearthmap/readings.h"

namespace hearthmap::test {

/// 2026-01-15T09:00:00Z, the `now` of every examples/flat readings fixture.
inline constexpr Timestamp kFlatNow = 1768467600;

/// examples/flat/config.json.
Config flat_config();

/// examples/flat/readings/<name>.json.
Readings flat_all_ok();
Readings flat_thirsty_many();
Readings flat_stale_sensors();
Readings flat_humid_bathroom();

/// A numeric reading.
Reading value_reading(double value, Timestamp last_updated, std::string unit = "");

/// Index of the pin or room with `id` in `config`; fails the calling test when absent.
std::size_t pin_index(const Config& config, const std::string& id);
std::size_t room_index(const Config& config, const std::string& id);

}  // namespace hearthmap::test
