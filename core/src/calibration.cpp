// calibration.cpp — raw soil moisture -> thirst, and the per-plant soil band (calibration.h).
#include "hearthmap/calibration.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace hearthmap {

std::optional<double> moisture_fraction(const Calibration& calibration, double raw) {
  const double width = calibration.wet - calibration.dry;
  // Written as !(>=) so a NaN width counts as degenerate too.
  if (!(std::fabs(width) >= kMinCalibrationWidth)) return std::nullopt;
  if (!std::isfinite(raw) || !std::isfinite(width)) return std::nullopt;  // also inf dry/wet
  const double f = (raw - calibration.dry) / width;
  return std::clamp(f, 0.0, 1.0);
}

std::optional<double> thirst(const Calibration& calibration, double raw) {
  const std::optional<double> f = moisture_fraction(calibration, raw);
  if (!f) return std::nullopt;
  return 1.0 - *f;
}

Band effective_soil_band(const Band& layer, const Calibration& calibration) {
  // Degenerate calibration: probe with any finite value; nullopt means the span is unusable.
  if (!moisture_fraction(calibration, calibration.dry)) return layer;

  const std::optional<double> dry_edge =
      calibration.happy_min ? thirst(calibration, *calibration.happy_min) : std::nullopt;
  const std::optional<double> wet_edge =
      calibration.happy_max ? thirst(calibration, *calibration.happy_max) : std::nullopt;
  if (!dry_edge && !wet_edge) return layer;

  Band b = layer;
  // 1. happy_min: the dry-side edge.
  if (dry_edge) {
    const double t = *dry_edge;
    b.comfort_max = t;
    b.warn_above = t;
    b.danger_above =
        layer.danger_above ? std::optional<double>(std::max(*layer.danger_above, t)) : std::nullopt;
  }
  // 2. happy_max: the wet-side edge.
  if (wet_edge) {
    const double t = *wet_edge;
    b.comfort_min = t;
    b.warn_below = t;
    b.danger_below =
        layer.danger_below ? std::optional<double>(std::min(*layer.danger_below, t)) : std::nullopt;
  }
  // 3. Repair the side that was not overridden.
  if (b.comfort_min > b.comfort_max) {
    if (dry_edge && !wet_edge) b.comfort_min = b.comfort_max;
    if (wet_edge && !dry_edge) b.comfort_max = b.comfort_min;
  }
  double lower = b.comfort_min;
  if (b.warn_below) {
    b.warn_below = std::min(*b.warn_below, lower);
    lower = *b.warn_below;
  }
  if (b.danger_below) b.danger_below = std::min(*b.danger_below, lower);
  double upper = b.comfort_max;
  if (b.warn_above) {
    b.warn_above = std::max(*b.warn_above, upper);
    upper = *b.warn_above;
  }
  if (b.danger_above) b.danger_above = std::max(*b.danger_above, upper);

  // 4. Widen the scale to contain everything.
  for (const std::optional<double>& v :
       {std::optional<double>(b.comfort_min), std::optional<double>(b.comfort_max), b.warn_below,
        b.warn_above, b.danger_below, b.danger_above}) {
    if (!v) continue;
    b.scale_min = std::min(b.scale_min, *v);
    b.scale_max = std::max(b.scale_max, *v);
  }
  return b;
}

}  // namespace hearthmap
