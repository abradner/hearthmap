#include "summary.h"

#include <cmath>
#include <cstdio>

#include "hearthmap/severity.h"

namespace hearthmap::host {

namespace {

struct Mean {
  double sum = 0.0;
  int count = 0;
  void add(double v) {
    sum += v;
    ++count;
  }
  double value() const { return sum / count; }
};

bool indoor(const Config& config, const Pin& pin) {
  for (const auto& r : config.rooms) {
    if (r.id == pin.room_id) return !r.outdoor;
  }
  return false;
}

}  // namespace

std::string house_summary(const Config& config, const Readings& readings, Timestamp now) {
  Mean temp;
  Mean hum;
  Mean pm;
  for (const Pin& pin : config.pins) {
    if (!indoor(config, pin)) continue;
    for (Role role : {Role::Temperature, Role::Humidity, Role::Pm25}) {
      if (role != Role::Pm25 && !feeds_layer(pin.kind, role)) continue;
      const auto a = assess(config, pin, role, readings, now);
      if (!a || a->freshness != Freshness::Fresh || !a->value) continue;
      (role == Role::Temperature ? temp : role == Role::Humidity ? hum : pm).add(*a->value);
    }
  }
  std::string out = "House";
  char buf[64];
  bool any = false;
  if (temp.count > 0) {
    std::snprintf(buf, sizeof buf, " %.1f\xC2\xB0", temp.value());
    out += buf;
    any = true;
  }
  if (hum.count > 0) {
    std::snprintf(buf, sizeof buf, "%s%.0f%% RH", any ? "   " : " ", hum.value());
    out += buf;
    any = true;
  }
  if (pm.count > 0) {
    std::snprintf(buf, sizeof buf, "%sPM2.5 %.0f", any ? "   " : " ", pm.value());
    out += buf;
    any = true;
  }
  return any ? out : "House: no data";
}

std::string clock_text(Timestamp now, int utc_offset_minutes) {
  const std::int64_t local = now + static_cast<std::int64_t>(utc_offset_minutes) * 60;
  std::int64_t of_day = local % 86400;
  if (of_day < 0) of_day += 86400;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", static_cast<int>(of_day / 3600),
                static_cast<int>((of_day % 3600) / 60));
  return buf;
}

}  // namespace hearthmap::host
