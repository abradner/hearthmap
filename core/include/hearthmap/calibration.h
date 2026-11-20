// calibration.h — raw soil moisture -> thirst, against a sensor's own dry/wet range.
//
// Thirst is the soil layer's value domain: 0.0 = as wet as the sensor's calibrated "wet"
// (just watered), 1.0 = as dry as its calibrated "dry". A raw reading of 0 is a real reading
// (a bone-dry pot), never treated as missing: missing is a ReadingState, not a value.
#pragma once

#include <optional>

#include "hearthmap/config.h"

namespace hearthmap {

/// Width below which a calibration range is degenerate. validate() rejects such configs; the
/// functions below still define behaviour for them.
inline constexpr double kMinCalibrationWidth = 1e-9;

/// True when `calibration`'s dry..wet span can be used: dry and wet are finite, wet - dry is
/// finite (does not overflow), and |wet - dry| >= kMinCalibrationWidth. The one definition of a
/// usable span: validate() rejects a calibration exactly when this is false, and
/// moisture_fraction() returns std::nullopt for every raw value exactly when this is false, so a
/// config validate() accepts never reports BadCalibration. Happy edges are not part of the span
/// (validate() checks them separately).
bool calibration_span_usable(const Calibration& calibration);

/// Moisture as a fraction of the calibrated span: (raw - dry) / (wet - dry), clamped to 0..1.
///
/// - Inverted ranges (wet < dry, typical of capacitive ADC probes) need no special case: the
///   formula handles either direction, so a raw value at `wet` gives 1.0 and at `dry` gives 0.0.
/// - Readings beyond either end clamp (wetter than wet -> 1.0, drier than dry -> 0.0).
/// - Unusable span (calibration_span_usable() false: zero-width, non-finite dry or wet, or a
///   width that overflows): std::nullopt. Core reports the pin as Stale with reason
///   Freshness::BadCalibration rather than guess (severity.h).
/// - The span is closed: `dry` and `wet` themselves are inside it (validate() accepts a happy
///   edge equal to either).
/// - Non-finite `raw`: std::nullopt.
std::optional<double> moisture_fraction(const Calibration& calibration, double raw);

/// 1 - moisture_fraction(), with the same nullopt cases.
std::optional<double> thirst(const Calibration& calibration, double raw);

/// The soil band that applies to one plant or soil pin: the layer's band with the pin's happy
/// range folded in. Inside the happy range is comfort; the happy edges are the warn edges.
///
/// Algorithm (b starts as a copy of `layer`):
///   1. happy_min present, t = thirst(happy_min): b.comfort_max = t; b.warn_above = t;
///      b.danger_above = layer.danger_above ? max(*layer.danger_above, t) : nullopt.
///   2. happy_max present, t = thirst(happy_max): b.comfort_min = t; b.warn_below = t;
///      b.danger_below = layer.danger_below ? min(*layer.danger_below, t) : nullopt.
///   3. Repair the side that was not overridden so Band's ordering still holds:
///      if b.comfort_min > b.comfort_max, move the non-overridden comfort edge onto the other
///      (only happy_min given: comfort_min = comfort_max; only happy_max given: comfort_max =
///      comfort_min). Then lower warn_below/danger_below to at most comfort_min/warn_below, and
///      raise warn_above/danger_above to at least comfort_max/warn_above, where present.
///   4. Widen scale_min/scale_max if needed to contain every value above.
/// Neither present, or the calibration degenerate (thirst() is nullopt): `layer` unchanged.
/// Both present: validate() guarantees happy_min is not wetter than happy_max, so step 3 has
/// nothing to repair on the comfort edges.
Band effective_soil_band(const Band& layer, const Calibration& calibration);

}  // namespace hearthmap
