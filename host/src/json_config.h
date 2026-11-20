// json_config.h — the host's ConfigSource (schema/hearthmap.schema.json -> core Config) and the
// readings-fixture parser (schema/readings.schema.json -> core Readings).
//
// The JSON library stays inside json_config.cpp: nothing here exposes it.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/readings.h"

namespace hearthmap::host {

/// Parses a config document. It applies the schema's defaults, records which band fields the
/// document set (LayerConfig::present), reports what only a parser can see (malformed JSON, a
/// wrong type, an unknown field, an unknown enum value) at its JSON-pointer path, and then runs
/// core's validate(): any Error there means no Config. It enforces no rule validate() owns.
class JsonConfigSource final : public ConfigSource {
 public:
  explicit JsonConfigSource(std::string text) : text_(std::move(text)) {}
  ConfigLoadResult load() const override;

 private:
  std::string text_;
};

/// RFC 3339 date-time ("2026-01-15T09:00:00Z", "...+10:00", optional fractional seconds,
/// which are dropped) to seconds since the epoch. nullopt when it does not parse.
std::optional<Timestamp> parse_rfc3339(const std::string& text);

struct ReadingsFixture {
  Readings readings;
  Timestamp now = 0;
};

struct ReadingsLoadResult {
  std::optional<ReadingsFixture> fixture;
  std::string error;  ///< Set when fixture is empty.
};

/// Parses a readings fixture (examples/README.md, "Readings fixtures"). Timestamps are passed
/// through as given (docs/decisions.md, freshness ruling: the host never re-stamps). A numeric
/// value becomes ReadingState::Value, "unavailable" Unavailable, "unknown" Unknown.
ReadingsLoadResult parse_readings(const std::string& text);

}  // namespace hearthmap::host
