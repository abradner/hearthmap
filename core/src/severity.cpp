// severity.cpp — freshness, classification and the per-binding assessment (severity.h).
#include "hearthmap/severity.h"

#include <cmath>
#include <cstdint>
#include <optional>

#include "hearthmap/calibration.h"

namespace hearthmap {
namespace {

const Room* find_room(const Config& config, const std::string& id) {
  for (const auto& r : config.rooms) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

// Indoor comfort does not apply outdoors (docs/decisions.md D-006). Moisture still does.
bool exempt_outdoors(Role role) {
  return role == Role::Temperature || role == Role::Humidity || role == Role::Co2;
}

}  // namespace

Freshness assess_freshness(const Reading* reading, Timestamp now, Seconds stale_after) {
  if (reading == nullptr) return Freshness::Missing;
  switch (reading->state) {
    case ReadingState::Unavailable: return Freshness::Unavailable;
    case ReadingState::Unknown: return Freshness::Unknown;
    case ReadingState::Value: break;
  }
  // Sources promise a finite value; if one slips through it is not a number we can show.
  if (!std::isfinite(reading->value)) return Freshness::Unknown;
  // A future last_updated (clock skew) is age 0. Otherwise the age is computed in unsigned
  // arithmetic, which is exact for any last_updated < now and cannot overflow.
  if (reading->last_updated >= now) return stale_after < 0 ? Freshness::TooOld : Freshness::Fresh;
  if (stale_after < 0) return Freshness::TooOld;
  const std::uint64_t age =
      static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(reading->last_updated);
  return age > static_cast<std::uint64_t>(stale_after) ? Freshness::TooOld : Freshness::Fresh;
}

Severity classify(const Band& band, double value) {
  if (band.danger_below && value < *band.danger_below) return Severity::Danger;
  if (band.danger_above && value > *band.danger_above) return Severity::Danger;
  if (band.warn_below && value < *band.warn_below) return Severity::Warn;
  if (band.warn_above && value > *band.warn_above) return Severity::Warn;
  return Severity::Ok;
}

std::optional<Assessment> assess(const Config& config, const Pin& pin, Role role,
                                 const Readings& readings, Timestamp now) {
  const EntityBinding* binding = find_binding(pin, role);
  if (binding == nullptr) return std::nullopt;

  Assessment a;
  a.role = role;
  const Reading* reading = readings.find(binding->entity_id);
  if (reading != nullptr && reading->state == ReadingState::Value &&
      std::isfinite(reading->value)) {
    a.raw = reading->value;
  }
  a.freshness = assess_freshness(reading, now, effective_stale_after(pin, config));

  const bool feeds = feeds_layer(pin.kind, role);
  if (feeds) {
    const Band& layer_band = config.layer(*layer_of(role)).band;
    a.band = (role == Role::Moisture && pin.calibration)
                 ? effective_soil_band(layer_band, *pin.calibration)
                 : layer_band;
  }

  // Moisture needs a usable calibration. Precedence (docs/decisions.md): Missing >
  // Unavailable > Unknown > BadCalibration > TooOld, so a degenerate (or, in an invalid config,
  // absent) calibration replaces only Fresh and TooOld.
  if (role == Role::Moisture) {
    const bool calibrated =
        pin.calibration && moisture_fraction(*pin.calibration, pin.calibration->dry).has_value();
    if (!calibrated) {
      if (a.freshness == Freshness::Fresh || a.freshness == Freshness::TooOld) {
        a.freshness = Freshness::BadCalibration;
      }
    } else if (a.freshness == Freshness::Fresh) {
      a.value = thirst(*pin.calibration, *a.raw);
    }
  } else if (a.freshness == Freshness::Fresh) {
    a.value = a.raw;
  }

  if (a.freshness != Freshness::Fresh) {
    a.value = std::nullopt;
    a.severity = Severity::Stale;
    return a;
  }

  // Not judged: roles that feed no layer, display-only roles, and roles whose layer is
  // disabled (D-020).
  if (!feeds || !config.layer(*layer_of(role)).enabled) {
    a.severity = Severity::Ok;
    return a;
  }
  const Room* room = find_room(config, pin.room_id);
  if (room != nullptr && room->outdoor && exempt_outdoors(role)) {
    a.severity = Severity::Ok;
    return a;
  }
  a.severity = classify(*a.band, *a.value);
  return a;
}

}  // namespace hearthmap
