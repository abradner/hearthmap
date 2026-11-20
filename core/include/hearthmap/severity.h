// severity.h — reading + band -> ok / warn / danger / stale.
//
// Two independent questions, answered separately and then combined:
//   freshness: can this reading be trusted at all? (assess_freshness)
//   severity:  given a trusted value, how bad is it? (classify)
#pragma once

#include <cstdint>
#include <optional>

#include "hearthmap/config.h"
#include "hearthmap/readings.h"

namespace hearthmap {

enum class Severity : std::uint8_t { Ok, Warn, Danger, Stale };

/// Ordering for "worst of": Ok < Stale < Warn < Danger. Stale ranks below Warn on purpose: a
/// silent sensor must be visible, but must not outrank a sensor reporting a real problem.
constexpr int rank(Severity s) {
  switch (s) {
    case Severity::Ok: return 0;
    case Severity::Stale: return 1;
    case Severity::Warn: return 2;
    case Severity::Danger: return 3;
  }
  return 0;
}

/// The worse of two severities by rank(); `a` on a tie.
constexpr Severity worse(Severity a, Severity b) {
  return rank(b) > rank(a) ? b : a;
}

/// Why a reading can or cannot be trusted. Every value except Fresh makes the severity Stale.
/// When several apply, the first in this precedence wins (docs/decisions.md D-012):
///   Missing > Unavailable > Unknown > BadCalibration > TooOld.
/// So an old reading on a degenerate calibration reports BadCalibration, the reason a human
/// can fix, rather than TooOld.
enum class Freshness : std::uint8_t {
  Fresh,
  Missing,         ///< The entity is absent from Readings (never received).
  Unavailable,     ///< HA said "unavailable".
  Unknown,         ///< HA said "unknown" or sent a non-numeric state.
  TooOld,          ///< now - last_updated > the pin's stale timeout.
  BadCalibration,  ///< Moisture with a degenerate calibration (calibration.h).
};

/// The stale timeout that applies to `pin`: its own override, else the config's.
inline Seconds effective_stale_after(const Pin& pin, const Config& config) {
  return pin.stale_after.value_or(config.stale_after);
}

/// Freshness of one reading, ignoring calibration (assess() layers BadCalibration on top, in
/// the precedence above).
/// - nullptr -> Missing; state Unavailable -> Unavailable; state Unknown -> Unknown.
/// - age = now - last_updated. age > stale_after -> TooOld. age == stale_after is still Fresh.
/// - A last_updated in the future (clock skew between HA and the panel) counts as age 0.
Freshness assess_freshness(const Reading* reading, Timestamp now, Seconds stale_after);

/// Severity of a trusted value against a band. Never returns Stale.
/// Order of tests: danger_below, danger_above, warn_below, warn_above, else Ok.
/// Comparisons are strict: value < danger_below -> Danger; value > danger_above -> Danger;
/// value < warn_below -> Warn; value > warn_above -> Warn. A value exactly on a threshold gets
/// the less severe side. Absent thresholds never match. The comfort band does not affect
/// severity (it only affects colour).
Severity classify(const Band& band, double value);

/// Everything core knows about one binding on one pin at one moment.
struct Assessment {
  Role role = Role::Temperature;
  Freshness freshness = Freshness::Missing;
  /// The value in the role's layer domain when Fresh: raw for most roles, thirst 0..1 for
  /// Moisture. nullopt whenever freshness != Fresh.
  std::optional<double> value;
  /// The raw reading when one with ReadingState::Value exists, fresh or not (overlay display).
  std::optional<double> raw;
  /// Stale when not Fresh. When Fresh: classify() against the band below for roles that feed a
  /// layer (feeds_layer()), except that in an outdoor room Temperature, Humidity and Co2 are
  /// always Ok (docs/decisions.md D-006). Ok, whatever the value, for: roles that feed no layer;
  /// display-only roles on soil/plant pins; roles whose layer is disabled (not judged: no badge,
  /// no pulse, D-020). Freshness is still reported for all of these, and `value` and `raw` are
  /// still filled, so the overlay shows them.
  Severity severity = Severity::Stale;
  /// The band that applies: the layer's band, or effective_soil_band() for Moisture. Filled for
  /// every role that feeds a layer (feeds_layer()), even when that layer is disabled or the
  /// reading is not Fresh, so the HUD can judge thirst on its own (view_model.h, Hud). nullopt
  /// for roles that feed no layer.
  std::optional<Band> band;
};

/// Assess the binding for `role` on `pin`. Returns nullopt when the pin does not bind `role`.
/// `pin` must belong to `config` (its room is looked up for the outdoor rule). Freshness follows
/// the precedence on Freshness: assess_freshness() first, then, for Moisture, a degenerate
/// calibration turns Fresh or TooOld into BadCalibration.
std::optional<Assessment> assess(const Config& config, const Pin& pin, Role role,
                                 const Readings& readings, Timestamp now);

}  // namespace hearthmap
