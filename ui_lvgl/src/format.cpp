#include "hearthmap/ui/format.h"

#include <cmath>
#include <cstdio>

#include "hearthmap/ui/theme.h"

namespace hearthmap::ui {

namespace {

std::string printf_string(const char* fmt, double value) {
  char buf[48];
  std::snprintf(buf, sizeof buf, fmt, value);
  return std::string(buf);
}

long round_half_away(double v) {
  return std::lround(v);
}

}  // namespace

std::uint32_t severity_colour(Severity severity) {
  switch (severity) {
    case Severity::Ok: return theme::kOk;
    case Severity::Warn: return theme::kWarn;
    case Severity::Danger: return theme::kDanger;
    case Severity::Stale: return theme::kStale;
  }
  return theme::kStale;
}

int moisture_percent(double thirst) {
  if (!std::isfinite(thirst)) return 0;
  const long p = round_half_away((1.0 - thirst) * 100.0);
  return static_cast<int>(p < 0 ? 0 : (p > 100 ? 100 : p));
}

int ring_permille(double thirst) {
  if (!std::isfinite(thirst)) return 30;
  const long p = round_half_away((1.0 - thirst) * 1000.0);
  return static_cast<int>(p < 30 ? 30 : (p > 1000 ? 1000 : p));
}

std::string format_value(Role role, double value) {
  switch (role) {
    case Role::Temperature: return printf_string("%.1f\xC2\xB0", value);
    case Role::Humidity:
    case Role::Battery: return printf_string("%.0f%%", value);
    case Role::Co2: return printf_string("%.0f ppm", value);
    case Role::Pm25: return printf_string("%.0f ug/m3", value);
    case Role::Voc: return printf_string("%.0f", value);
    case Role::Moisture: return std::to_string(moisture_percent(value)) + "%";
    case Role::PredictedDryAt: return std::string();
  }
  return std::string();
}

std::string role_label(Role role, PinKind kind) {
  const bool in_pot = kind == PinKind::Plant || kind == PinKind::Soil;
  switch (role) {
    case Role::Temperature: return in_pot ? "Soil temp" : "Temperature";
    case Role::Humidity: return in_pot ? "Air humidity" : "Humidity";
    case Role::Moisture: return "Moisture";
    case Role::Co2: return "CO2";
    case Role::Pm25: return "PM2.5";
    case Role::Voc: return "VOC";
    case Role::Battery: return "Battery";
    case Role::PredictedDryAt: return "Dry by";
  }
  return std::string();
}

std::string layer_label(Layer layer) {
  switch (layer) {
    case Layer::Temperature: return "Temp";
    case Layer::Humidity: return "Humidity";
    case Layer::Soil: return "Soil";
    case Layer::Air: return "Air";
  }
  return std::string();
}

std::string format_seen(Seconds age) {
  if (age < 60) return "seen just now";
  const Seconds minutes = age / 60;
  if (minutes < 120) return "seen " + std::to_string(minutes) + " min ago";
  const Seconds hours = minutes / 60;
  if (hours < 48) return "seen " + std::to_string(hours) + " h ago";
  return "seen " + std::to_string(hours / 24) + " d ago";
}

std::string thirst_verdict(const Assessment& moisture) {
  if (moisture.freshness != Freshness::Fresh || !moisture.value.has_value()) return "no data";
  const double thirst = *moisture.value;
  const std::string pct = std::to_string(moisture_percent(thirst)) + "%  ";
  switch (moisture.severity) {
    case Severity::Ok: return pct + "happy";
    case Severity::Stale: return "no data";
    case Severity::Warn:
    case Severity::Danger: {
      const bool wet_side = moisture.band.has_value() && thirst < moisture.band->comfort_min;
      if (wet_side) return pct + "too wet";
      return pct + (moisture.severity == Severity::Danger ? "water now" : "water soon");
    }
  }
  return pct;
}

}  // namespace hearthmap::ui
