#include "json_config.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace hearthmap::host {

namespace {

using nlohmann::json;

class Parser {
 public:
  std::vector<ConfigIssue> issues;

  void error(const std::string& path, const std::string& message) {
    issues.push_back(ConfigIssue{IssueSeverity::Error, path.empty() ? "/" : path, message});
  }

  // Reports every key of `obj` not in `allowed`.
  void only(const json& obj, const std::string& path,
            std::initializer_list<std::string_view> allowed) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
      bool ok = false;
      for (auto a : allowed) ok = ok || it.key() == a;
      if (!ok) error(path + "/" + it.key(), "unknown field");
    }
  }

  const json* member(const json& obj, const char* key) {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
  }

  bool object_at(const json& v, const std::string& path) {
    if (v.is_object()) return true;
    error(path, "expected an object");
    return false;
  }

  std::optional<double> number(const json& v, const std::string& path) {
    if (v.is_number()) return v.get<double>();
    error(path, "expected a number");
    return std::nullopt;
  }

  // A JSON integer that fits [lo, hi]. As in JSON Schema, a number spelled with a fraction or
  // exponent counts when it is mathematically integral (1280.0, 4.8e1); 48.5 does not. A value
  // outside [lo, hi] is an error here, never narrowed into a different valid value.
  std::optional<std::int64_t> integer(const json& v, const std::string& path,
                                      std::int64_t lo = std::numeric_limits<std::int32_t>::min(),
                                      std::int64_t hi = std::numeric_limits<std::int32_t>::max()) {
    std::int64_t out = 0;
    if (v.is_number_unsigned()) {
      const auto u = v.get<std::uint64_t>();
      if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        error(path, "integer out of range");
        return std::nullopt;
      }
      out = static_cast<std::int64_t>(u);
    } else if (v.is_number_integer()) {
      out = v.get<std::int64_t>();
    } else if (v.is_number_float()) {
      const double d = v.get<double>();
      if (!std::isfinite(d) || std::trunc(d) != d) {
        error(path, "expected an integer");
        return std::nullopt;
      }
      // 2^63 is exactly representable; anything at or beyond it cannot be an int64.
      if (d >= 9223372036854775808.0 || d < -9223372036854775808.0) {
        error(path, "integer out of range");
        return std::nullopt;
      }
      out = static_cast<std::int64_t>(d);
    } else {
      error(path, "expected an integer");
      return std::nullopt;
    }
    if (out < lo || out > hi) {
      error(path, "integer out of range");
      return std::nullopt;
    }
    return out;
  }

  static constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
  static constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();

  std::optional<std::string> string(const json& v, const std::string& path) {
    if (v.is_string()) return v.get<std::string>();
    error(path, "expected a string");
    return std::nullopt;
  }

  std::optional<bool> boolean(const json& v, const std::string& path) {
    if (v.is_boolean()) return v.get<bool>();
    error(path, "expected true or false");
    return std::nullopt;
  }

  std::optional<MapPoint> point(const json& v, const std::string& path) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
      error(path, "expected [x, y]");
      return std::nullopt;
    }
    return MapPoint{v[0].get<double>(), v[1].get<double>()};
  }

  // Required member: reports its absence.
  const json* required(const json& obj, const char* key, const std::string& path) {
    const json* m = member(obj, key);
    if (m == nullptr) error(path + "/" + key, "required field missing");
    return m;
  }

  template <typename T>
  static T narrow(std::int64_t v) {
    return static_cast<T>(v);
  }

  void canvas(const json& j, Canvas& c) {
    const std::string path = "/canvas";
    if (!object_at(j, path)) return;
    only(j, path, {"width", "height", "orientation", "map_area"});
    if (auto* w = required(j, "width", path)) {
      if (auto v = integer(*w, path + "/width")) c.width = narrow<std::int32_t>(*v);
    }
    if (auto* h = required(j, "height", path)) {
      if (auto v = integer(*h, path + "/height")) c.height = narrow<std::int32_t>(*v);
    }
    if (auto* o = required(j, "orientation", path)) {
      if (auto v = string(*o, path + "/orientation")) {
        if (*v == "landscape") {
          c.orientation = Orientation::Landscape;
        } else if (*v == "portrait") {
          c.orientation = Orientation::Portrait;
        } else {
          error(path + "/orientation", "expected \"landscape\" or \"portrait\"");
        }
      }
    }
    // Schema default: the whole canvas (D-023).
    c.map_area = CanvasRect{0, 0, c.width, c.height};
    if (auto* m = member(j, "map_area")) {
      const std::string mp = path + "/map_area";
      if (object_at(*m, mp)) {
        only(*m, mp, {"x", "y", "width", "height"});
        std::int32_t* fields[] = {&c.map_area.x, &c.map_area.y, &c.map_area.width,
                                  &c.map_area.height};
        const char* names[] = {"x", "y", "width", "height"};
        for (int i = 0; i < 4; ++i) {
          if (auto* f = required(*m, names[i], mp)) {
            if (auto v = integer(*f, mp + "/" + names[i])) *fields[i] = narrow<std::int32_t>(*v);
          }
        }
      }
    }
  }

  void room(const json& j, const std::string& path, Room& r) {
    if (!object_at(j, path)) return;
    only(j, path, {"id", "name", "polygon", "outdoor"});
    if (auto* v = required(j, "id", path)) r.id = string(*v, path + "/id").value_or("");
    if (auto* v = required(j, "name", path)) r.name = string(*v, path + "/name").value_or("");
    if (auto* v = required(j, "polygon", path)) {
      if (!v->is_array()) {
        error(path + "/polygon", "expected an array of points");
      } else {
        for (std::size_t i = 0; i < v->size(); ++i) {
          if (auto p = point((*v)[i], path + "/polygon/" + std::to_string(i))) {
            r.polygon.push_back(*p);
          }
        }
      }
    }
    if (auto* v = member(j, "outdoor")) r.outdoor = boolean(*v, path + "/outdoor").value_or(false);
  }

  static std::optional<PinKind> kind_of(const std::string& s) {
    if (s == "temperature") return PinKind::Temperature;
    if (s == "humidity") return PinKind::Humidity;
    if (s == "soil") return PinKind::Soil;
    if (s == "air_quality") return PinKind::AirQuality;
    if (s == "plant") return PinKind::Plant;
    return std::nullopt;
  }

  static std::optional<Role> role_of(const std::string& s) {
    if (s == "temperature") return Role::Temperature;
    if (s == "humidity") return Role::Humidity;
    if (s == "moisture") return Role::Moisture;
    if (s == "co2") return Role::Co2;
    if (s == "pm25") return Role::Pm25;
    if (s == "voc") return Role::Voc;
    if (s == "battery") return Role::Battery;
    if (s == "predicted_dry_at") return Role::PredictedDryAt;
    return std::nullopt;
  }

  void pin(const json& j, const std::string& path, Pin& p) {
    if (!object_at(j, path)) return;
    only(j, path,
         {"id", "kind", "name", "position", "room", "entities", "photo", "calibration",
          "stale_after_s"});
    if (auto* v = required(j, "id", path)) p.id = string(*v, path + "/id").value_or("");
    if (auto* v = required(j, "kind", path)) {
      if (auto s = string(*v, path + "/kind")) {
        if (auto k = kind_of(*s)) {
          p.kind = *k;
        } else {
          error(path + "/kind", "unknown pin kind");
        }
      }
    }
    if (auto* v = required(j, "name", path)) p.name = string(*v, path + "/name").value_or("");
    if (auto* v = required(j, "position", path)) {
      if (auto pt = point(*v, path + "/position")) p.position = *pt;
    }
    if (auto* v = required(j, "room", path)) p.room_id = string(*v, path + "/room").value_or("");
    if (auto* v = required(j, "entities", path)) {
      const std::string ep = path + "/entities";
      if (object_at(*v, ep)) {
        for (auto it = v->begin(); it != v->end(); ++it) {
          const auto role = role_of(it.key());
          if (!role) {
            error(ep + "/" + it.key(), "unknown role");
            continue;
          }
          if (auto id = string(it.value(), ep + "/" + it.key())) {
            p.entities.push_back(EntityBinding{*role, *id});
          }
        }
      }
    }
    if (auto* v = member(j, "photo")) {
      if (auto s = string(*v, path + "/photo")) p.photo = *s;
    }
    if (auto* v = member(j, "calibration")) {
      const std::string cp = path + "/calibration";
      if (object_at(*v, cp)) {
        only(*v, cp, {"dry", "wet", "happy_min", "happy_max"});
        Calibration c;
        if (auto* d = required(*v, "dry", cp)) c.dry = number(*d, cp + "/dry").value_or(0.0);
        if (auto* w = required(*v, "wet", cp)) c.wet = number(*w, cp + "/wet").value_or(0.0);
        if (auto* h = member(*v, "happy_min")) c.happy_min = number(*h, cp + "/happy_min");
        if (auto* h = member(*v, "happy_max")) c.happy_max = number(*h, cp + "/happy_max");
        p.calibration = c;
      }
    }
    if (auto* v = member(j, "stale_after_s")) {
      if (auto s = integer(*v, path + "/stale_after_s", kI64Min, kI64Max)) p.stale_after = *s;
    }
  }

  void band_layer(const json& j, const std::string& path, LayerConfig& lc) {
    if (!object_at(j, path)) return;
    only(j, path,
         {"enabled", "comfort_min", "comfort_max", "warn_below", "warn_above", "danger_below",
          "danger_above", "scale_min", "scale_max"});
    if (auto* v = member(j, "enabled")) lc.enabled = boolean(*v, path + "/enabled").value_or(true);
    struct Req {
      const char* key;
      BandField field;
      double* target;
    };
    for (const Req& r : {Req{"comfort_min", BandField::ComfortMin, &lc.band.comfort_min},
                         Req{"comfort_max", BandField::ComfortMax, &lc.band.comfort_max},
                         Req{"scale_min", BandField::ScaleMin, &lc.band.scale_min},
                         Req{"scale_max", BandField::ScaleMax, &lc.band.scale_max}}) {
      if (auto* v = member(j, r.key)) {
        lc.present = static_cast<std::uint8_t>(lc.present | band_field_bit(r.field));
        if (auto n = number(*v, path + "/" + r.key)) *r.target = *n;
      }
    }
    struct Opt {
      const char* key;
      BandField field;
      std::optional<double>* target;
    };
    for (const Opt& o : {Opt{"warn_below", BandField::WarnBelow, &lc.band.warn_below},
                         Opt{"warn_above", BandField::WarnAbove, &lc.band.warn_above},
                         Opt{"danger_below", BandField::DangerBelow, &lc.band.danger_below},
                         Opt{"danger_above", BandField::DangerAbove, &lc.band.danger_above}}) {
      if (auto* v = member(j, o.key)) {
        lc.present = static_cast<std::uint8_t>(lc.present | band_field_bit(o.field));
        if (v->is_null()) {
          o.target->reset();  // null removes the threshold
        } else if (auto n = number(*v, path + "/" + o.key)) {
          *o.target = *n;
        }
      }
    }
  }

  static std::optional<Layer> layer_of_name(const std::string& s) {
    if (s == "temperature") return Layer::Temperature;
    if (s == "humidity") return Layer::Humidity;
    if (s == "soil") return Layer::Soil;
    if (s == "air") return Layer::Air;
    return std::nullopt;
  }

  void layers(const json& j, Config& c) {
    const std::string path = "/layers";
    if (!object_at(j, path)) return;
    only(j, path, {"default", "temperature", "humidity", "soil", "air"});
    if (auto* v = member(j, "default")) {
      if (auto s = string(*v, path + "/default")) {
        if (auto l = layer_of_name(*s)) {
          c.default_layer = *l;
        } else {
          error(path + "/default", "unknown layer");
        }
      }
    }
    for (const char* name : {"temperature", "humidity", "soil", "air"}) {
      if (auto* v = member(j, name)) {
        band_layer(*v, path + "/" + name, c.layers[index_of(*layer_of_name(name))]);
      }
    }
  }

  void config(const json& j, Config& c) {
    if (!object_at(j, "")) return;
    only(j, "",
         {"$schema", "version", "canvas", "temperature_unit", "rooms", "pins", "layers",
          "stale_after_s", "hud", "heat"});
    if (auto* v = required(j, "version", "")) {
      if (auto n = integer(*v, "/version", std::numeric_limits<int>::min(),
                           std::numeric_limits<int>::max())) {
        c.version = static_cast<int>(*n);
      }
    }
    if (auto* v = required(j, "canvas", "")) canvas(*v, c.canvas);
    if (auto* v = member(j, "temperature_unit")) {
      if (auto s = string(*v, "/temperature_unit")) {
        if (*s == "C") {
          c.temperature_unit = TemperatureUnit::Celsius;
        } else if (*s == "F") {
          c.temperature_unit = TemperatureUnit::Fahrenheit;
        } else {
          error("/temperature_unit", "expected \"C\" or \"F\"");
        }
      }
    }
    if (auto* v = required(j, "rooms", "")) {
      if (!v->is_array()) {
        error("/rooms", "expected an array");
      } else {
        for (std::size_t i = 0; i < v->size(); ++i) {
          Room r;
          room((*v)[i], "/rooms/" + std::to_string(i), r);
          c.rooms.push_back(std::move(r));
        }
      }
    }
    if (auto* v = required(j, "pins", "")) {
      if (!v->is_array()) {
        error("/pins", "expected an array");
      } else {
        for (std::size_t i = 0; i < v->size(); ++i) {
          Pin p;
          pin((*v)[i], "/pins/" + std::to_string(i), p);
          c.pins.push_back(std::move(p));
        }
      }
    }
    if (auto* v = member(j, "layers")) layers(*v, c);
    if (auto* v = member(j, "stale_after_s")) {
      if (auto n = integer(*v, "/stale_after_s", kI64Min, kI64Max)) c.stale_after = *n;
    }
    if (auto* v = member(j, "hud")) {
      if (object_at(*v, "/hud")) {
        only(*v, "/hud", {"max_items"});
        if (auto* m = member(*v, "max_items")) {
          if (auto n = integer(*m, "/hud/max_items")) c.hud.max_items = narrow<std::int32_t>(*n);
        }
      }
    }
    if (auto* v = member(j, "heat")) {
      if (object_at(*v, "/heat")) {
        only(*v, "/heat", {"grid_cols", "grid_rows", "idw_power"});
        if (auto* m = member(*v, "grid_cols")) {
          if (auto n = integer(*m, "/heat/grid_cols")) c.heat.grid_cols = narrow<std::int32_t>(*n);
        }
        if (auto* m = member(*v, "grid_rows")) {
          if (auto n = integer(*m, "/heat/grid_rows")) c.heat.grid_rows = narrow<std::int32_t>(*n);
        }
        if (auto* m = member(*v, "idw_power")) {
          if (auto n = number(*m, "/heat/idw_power")) c.heat.idw_power = *n;
        }
      }
    }
  }
};

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9U)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace

ConfigLoadResult JsonConfigSource::load() const {
  ConfigLoadResult result;
  json doc;
  try {
    doc = json::parse(text_);
  } catch (const json::parse_error& e) {
    result.issues.push_back(
        ConfigIssue{IssueSeverity::Error, "/", std::string("malformed JSON: ") + e.what()});
    return result;
  }
  Parser p;
  Config c;
  p.config(doc, c);
  result.issues = std::move(p.issues);
  if (!usable(result.issues)) return result;
  std::vector<ConfigIssue> semantic = validate(c);
  result.issues.insert(result.issues.end(), semantic.begin(), semantic.end());
  if (usable(result.issues)) result.config = std::move(c);
  return result;
}

std::optional<Timestamp> parse_rfc3339(const std::string& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  int consumed = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &consumed) !=
          6 ||
      consumed != 19) {
    return std::nullopt;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return std::nullopt;
  std::size_t i = 19;
  if (i < text.size() && text[i] == '.') {
    ++i;
    const std::size_t start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == start) return std::nullopt;
  }
  std::int64_t offset = 0;
  if (i < text.size() && (text[i] == 'Z' || text[i] == 'z')) {
    ++i;
  } else if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    int oh = 0, om = 0, n = 0;
    if (std::sscanf(text.c_str() + i + 1, "%2d:%2d%n", &oh, &om, &n) != 2 || n != 5) {
      return std::nullopt;
    }
    offset = (text[i] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    i += 6;
  } else {
    return std::nullopt;
  }
  if (i != text.size()) return std::nullopt;
  const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return days * 86400 + h * 3600 + mi * 60 + s - offset;
}

ReadingsLoadResult parse_readings(const std::string& text) {
  ReadingsLoadResult result;
  json doc;
  try {
    doc = json::parse(text);
  } catch (const json::parse_error& e) {
    result.error = std::string("malformed JSON: ") + e.what();
    return result;
  }
  if (!doc.is_object() || !doc.contains("now") || !doc["now"].is_string() ||
      !doc.contains("readings") || !doc["readings"].is_object()) {
    result.error = "a readings fixture needs \"now\" (a date-time) and \"readings\" (an object)";
    return result;
  }
  ReadingsFixture f;
  const auto now = parse_rfc3339(doc["now"].get<std::string>());
  if (!now) {
    result.error = "/now: not an RFC 3339 date-time";
    return result;
  }
  f.now = *now;
  for (auto it = doc["readings"].begin(); it != doc["readings"].end(); ++it) {
    const std::string path = "/readings/" + it.key();
    const json& r = it.value();
    if (!r.is_object() || !r.contains("value") || !r.contains("last_updated") ||
        !r["last_updated"].is_string()) {
      result.error = path + ": needs \"value\" and \"last_updated\"";
      return result;
    }
    Reading reading;
    const json& v = r["value"];
    if (v.is_number()) {
      const double x = v.get<double>();
      if (std::isfinite(x)) {
        reading.state = ReadingState::Value;
        reading.value = x;
      } else {
        reading.state = ReadingState::Unknown;
      }
    } else if (v.is_string() && v.get<std::string>() == "unavailable") {
      reading.state = ReadingState::Unavailable;
    } else if (v.is_string() && v.get<std::string>() == "unknown") {
      reading.state = ReadingState::Unknown;
    } else {
      result.error = path + "/value: a number, \"unavailable\" or \"unknown\"";
      return result;
    }
    if (r.contains("unit") && r["unit"].is_string()) reading.unit = r["unit"].get<std::string>();
    const auto t = parse_rfc3339(r["last_updated"].get<std::string>());
    if (!t) {
      result.error = path + "/last_updated: not an RFC 3339 date-time";
      return result;
    }
    reading.last_updated = *t;
    f.readings.by_entity.emplace(it.key(), std::move(reading));
  }
  result.fixture = std::move(f);
  return result;
}

}  // namespace hearthmap::host
