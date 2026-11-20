// config.cpp — validate(): the semantic checks the JSON Schema cannot express (config.h).
//
// Every check appends to one list and carries on, so a single pass reports every problem the
// editor or the host needs to show. Paths are JSON-pointer style and use the schema's field
// names ("/pins/3/room", "/stale_after_s"), not the C++ member names.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hearthmap/calibration.h"
#include "hearthmap/config.h"
#include "hearthmap/geometry.h"

namespace hearthmap {
namespace {

const char* layer_key(Layer layer) {
  switch (layer) {
    case Layer::Temperature: return "temperature";
    case Layer::Humidity: return "humidity";
    case Layer::Soil: return "soil";
    case Layer::Air: return "air";
  }
  return "unknown";
}

constexpr Layer kAllLayers[] = {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air};

class Issues {
 public:
  void error(std::string path, std::string message) {
    list_.push_back(ConfigIssue{IssueSeverity::Error, std::move(path), std::move(message)});
  }
  void warning(std::string path, std::string message) {
    list_.push_back(ConfigIssue{IssueSeverity::Warning, std::move(path), std::move(message)});
  }
  std::vector<ConfigIssue> take() { return std::move(list_); }

 private:
  std::vector<ConfigIssue> list_;
};

std::string index_path(const char* collection, std::size_t i) {
  return std::string("/") + collection + "/" + std::to_string(i);
}

// --- schema ranges (the schema checks these too, but nothing may rely on it having run) -----

constexpr std::size_t kMaxRooms = 64;
constexpr std::size_t kMaxPins = 256;
constexpr std::size_t kMinVertices = 3;
constexpr std::size_t kMaxVertices = 64;
constexpr std::size_t kMaxIdLength = 48;
constexpr std::size_t kMaxNameLength = 48;
constexpr std::int32_t kMaxCanvasSide = 4096;
constexpr std::int32_t kMaxGridSide = 256;
constexpr double kMaxIdwPower = 8.0;
constexpr std::int32_t kMaxHudItems = 20;

bool lower_alpha(char c) {
  return c >= 'a' && c <= 'z';
}
bool digit(char c) {
  return c >= '0' && c <= '9';
}

// ^[a-z][a-z0-9_]{0,47}$
bool valid_id(const std::string& s) {
  if (s.empty() || s.size() > kMaxIdLength || !lower_alpha(s[0])) return false;
  for (char c : s) {
    if (!lower_alpha(c) && !digit(c) && c != '_') return false;
  }
  return true;
}

// ^[a-z0-9][a-z0-9_-]{0,47}$
bool valid_photo(const std::string& s) {
  if (s.empty() || s.size() > kMaxIdLength || !(lower_alpha(s[0]) || digit(s[0]))) return false;
  for (char c : s) {
    if (!lower_alpha(c) && !digit(c) && c != '_' && c != '-') return false;
  }
  return true;
}

// ^[a-z0-9_]+\.[a-z0-9_]+$
bool valid_entity_id(const std::string& s) {
  const std::size_t dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == s.size()) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == dot) continue;
    const char c = s[i];
    if (!lower_alpha(c) && !digit(c) && c != '_') return false;
  }
  return true;
}

// The number of Unicode code points in `s` if it is well-formed UTF-8 (no overlong forms, no
// surrogates, nothing above U+10FFFF, no truncated sequence), else nullopt. JSON Schema's
// maxLength counts code points, so this is what a name's length means (config.h, validate()).
std::optional<std::size_t> utf8_code_points(const std::string& s) {
  std::size_t count = 0;
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if (b0 < 0x80U) {
      len = 1;
      cp = b0;
    } else if ((b0 & 0xE0U) == 0xC0U) {
      len = 2;
      cp = b0 & 0x1FU;
      min = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
      len = 3;
      cp = b0 & 0x0FU;
      min = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
      len = 4;
      cp = b0 & 0x07U;
      min = 0x10000U;
    } else {
      return std::nullopt;  // a continuation byte where a sequence should start, or 0xF8..0xFF
    }
    if (len > n - i) return std::nullopt;  // truncated
    for (std::size_t k = 1; k < len; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if ((b & 0xC0U) != 0x80U) return std::nullopt;
      cp = (cp << 6U) | (b & 0x3FU);
    }
    if (cp < min) return std::nullopt;                        // overlong
    if (cp >= 0xD800U && cp <= 0xDFFFU) return std::nullopt;  // surrogate
    if (cp > 0x10FFFFU) return std::nullopt;
    i += len;
    ++count;
  }
  return count;
}

bool valid_name(const std::string& s) {
  const std::optional<std::size_t> length = utf8_code_points(s);
  return length && *length >= 1 && *length <= kMaxNameLength;
}

bool in_unit_square(MapPoint p) {
  return p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0;  // false for NaN
}

const char* role_key(Role role) {
  switch (role) {
    case Role::Temperature: return "temperature";
    case Role::Humidity: return "humidity";
    case Role::Moisture: return "moisture";
    case Role::Co2: return "co2";
    case Role::Pm25: return "pm25";
    case Role::Voc: return "voc";
    case Role::Battery: return "battery";
    case Role::PredictedDryAt: return "predicted_dry_at";
  }
  return "unknown";
}

// --- polygons ------------------------------------------------------------------------------

double cross(MapPoint o, MapPoint a, MapPoint b) {
  return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

int orientation(MapPoint o, MapPoint a, MapPoint b) {
  const double c = cross(o, a, b);
  if (c > 0.0) return 1;
  if (c < 0.0) return -1;
  return 0;
}

// p is collinear with segment ab; is it within ab's bounding box (on the segment)?
bool on_segment(MapPoint a, MapPoint b, MapPoint p) {
  return std::fmin(a.x, b.x) <= p.x && p.x <= std::fmax(a.x, b.x) && std::fmin(a.y, b.y) <= p.y &&
         p.y <= std::fmax(a.y, b.y);
}

// Closed segments ab and cd share at least one point.
bool segments_touch(MapPoint a, MapPoint b, MapPoint c, MapPoint d) {
  const int o1 = orientation(a, b, c);
  const int o2 = orientation(a, b, d);
  const int o3 = orientation(c, d, a);
  const int o4 = orientation(c, d, b);
  if (o1 != o2 && o3 != o4 && o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0) return true;
  if (o1 == 0 && on_segment(a, b, c)) return true;
  if (o2 == 0 && on_segment(a, b, d)) return true;
  if (o3 == 0 && on_segment(c, d, a)) return true;
  if (o4 == 0 && on_segment(c, d, b)) return true;
  return false;
}

// A polygon is simple when its edges meet only where consecutive edges share a vertex, and
// consecutive edges do not fold back over each other. Repeated vertices (including repeating
// the first vertex at the end, which the schema forbids) fail those same tests (a zero-length
// edge touches a non-adjacent one or folds back), as does a non-finite coordinate. Caller
// guarantees >= 3 vertices.
bool is_simple(const Polygon& poly) {
  const std::size_t n = poly.size();
  for (const auto& v : poly) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const MapPoint a = poly[i];
    const MapPoint b = poly[(i + 1) % n];
    // Consecutive edge (b, c): collinear and turning back means the edges overlap.
    const MapPoint c = poly[(i + 2) % n];
    if (orientation(a, b, c) == 0) {
      const double dot = (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y);
      if (dot < 0.0) return false;
    }
    // Every non-adjacent edge must not touch this one at all.
    for (std::size_t j = i + 2; j < n; ++j) {
      if (i == 0 && j == n - 1) continue;  // edges (n-1, 0) and (0, 1) are adjacent
      if (segments_touch(a, b, poly[j], poly[(j + 1) % n])) return false;
    }
  }
  return true;
}

// --- bands ---------------------------------------------------------------------------------

// Band ordering for every threshold present (config.h, Band). Written with `<=` so that a NaN
// anywhere fails the check rather than slipping through.
bool band_finite(const Band& b) {
  auto fin = [](const std::optional<double>& v) { return !v || std::isfinite(*v); };
  return std::isfinite(b.comfort_min) && std::isfinite(b.comfort_max) && fin(b.warn_below) &&
         fin(b.warn_above) && fin(b.danger_below) && fin(b.danger_above) &&
         std::isfinite(b.scale_min) && std::isfinite(b.scale_max);
}

bool band_ordered(const Band& b) {
  double prev = b.scale_min;
  bool ok = true;
  auto step = [&](double v) {
    ok = ok && prev <= v;
    prev = v;
  };
  if (b.danger_below) step(*b.danger_below);
  if (b.warn_below) step(*b.warn_below);
  step(b.comfort_min);
  step(b.comfort_max);
  if (b.warn_above) step(*b.warn_above);
  if (b.danger_above) step(*b.danger_above);
  step(b.scale_max);
  return ok && b.scale_min < b.scale_max;
}

// --- sections ------------------------------------------------------------------------------

void check_canvas(const Canvas& canvas, Issues& out) {
  if (canvas.width < 1 || canvas.width > kMaxCanvasSide) {
    out.error("/canvas/width", "canvas width must be 1..4096");
  }
  if (canvas.height < 1 || canvas.height > kMaxCanvasSide) {
    out.error("/canvas/height", "canvas height must be 1..4096");
  }
  if (canvas.orientation == Orientation::Landscape && canvas.width <= canvas.height) {
    out.error("/canvas/orientation", "landscape needs width > height");
  }
  if (canvas.orientation == Orientation::Portrait && canvas.height <= canvas.width) {
    out.error("/canvas/orientation", "portrait needs height > width");
  }
  const CanvasRect& m = canvas.map_area;
  const std::int64_t right = static_cast<std::int64_t>(m.x) + m.width;
  const std::int64_t bottom = static_cast<std::int64_t>(m.y) + m.height;
  if (m.x < 0 || m.y < 0 || m.width < 1 || m.height < 1 || right > canvas.width ||
      bottom > canvas.height) {
    out.error("/canvas/map_area", "map area must be non-empty and lie inside the canvas");
  }
}

void check_rooms(const Config& config, Issues& out) {
  if (config.rooms.empty()) out.error("/rooms", "a config needs at least one room");
  if (config.rooms.size() > kMaxRooms) out.error("/rooms", "at most 64 rooms");
  std::set<std::string> seen;
  for (std::size_t i = 0; i < config.rooms.size(); ++i) {
    const Room& room = config.rooms[i];
    const std::string base = index_path("rooms", i);
    if (!valid_id(room.id)) {
      out.error(base + "/id", "room id must match [a-z][a-z0-9_]{0,47}");
    } else if (!seen.insert(room.id).second) {
      out.error(base + "/id", "duplicate room id '" + room.id + "'");
    }
    if (!valid_name(room.name))
      out.error(base + "/name", "name must be 1..48 characters of valid UTF-8");
    bool vertices_in_range = true;
    for (std::size_t v = 0; v < room.polygon.size(); ++v) {
      if (!in_unit_square(room.polygon[v])) {
        out.error(base + "/polygon/" + std::to_string(v), "vertex must lie within 0..1");
        vertices_in_range = false;
      }
    }
    if (room.polygon.size() < kMinVertices || room.polygon.size() > kMaxVertices) {
      out.error(base + "/polygon", "a room polygon needs 3..64 vertices");
    } else if (vertices_in_range && !is_simple(room.polygon)) {
      out.error(base + "/polygon", "room polygon is not simple (edges cross, overlap or repeat)");
    }
  }
}

const Room* find_room(const Config& config, const std::string& id) {
  for (const auto& r : config.rooms) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

void check_calibration(const Calibration& cal, const std::string& base, Issues& out) {
  const double width = cal.wet - cal.dry;
  if (!(std::fabs(width) >= kMinCalibrationWidth)) {  // also rejects NaN
    out.error(base, "calibration dry and wet must differ");
    return;  // happy-range checks are meaningless on a degenerate span
  }
  // Position along dry (0) .. wet (1), unclamped, so "outside the span" is visible.
  auto along = [&](double raw) { return (raw - cal.dry) / width; };
  auto in_span = [&](double raw) {
    const double f = along(raw);
    return f >= 0.0 && f <= 1.0;  // false for NaN
  };
  bool min_ok = true;
  bool max_ok = true;
  if (cal.happy_min && !in_span(*cal.happy_min)) {
    out.error(base + "/happy_min", "happy_min must lie between dry and wet");
    min_ok = false;
  }
  if (cal.happy_max && !in_span(*cal.happy_max)) {
    out.error(base + "/happy_max", "happy_max must lie between dry and wet");
    max_ok = false;
  }
  if (cal.happy_min && cal.happy_max && min_ok && max_ok &&
      along(*cal.happy_min) > along(*cal.happy_max)) {
    out.error(base + "/happy_max", "happy_max must not be drier than happy_min");
  }
}

void check_pins(const Config& config, Issues& out) {
  if (config.pins.size() > kMaxPins) out.error("/pins", "at most 256 pins");
  std::set<std::string> seen;
  for (std::size_t i = 0; i < config.pins.size(); ++i) {
    const Pin& pin = config.pins[i];
    const std::string base = index_path("pins", i);
    if (!valid_id(pin.id)) {
      out.error(base + "/id", "pin id must match [a-z][a-z0-9_]{0,47}");
    } else if (!seen.insert(pin.id).second) {
      out.error(base + "/id", "duplicate pin id '" + pin.id + "'");
    }
    if (!valid_name(pin.name))
      out.error(base + "/name", "name must be 1..48 characters of valid UTF-8");
    const bool position_ok = in_unit_square(pin.position);
    if (!position_ok) out.error(base + "/position", "position must lie within 0..1");
    const bool soil_kind = pin.kind == PinKind::Soil || pin.kind == PinKind::Plant;
    for (const auto& b : pin.entities) {
      const std::string role_path = base + "/entities/" + role_key(b.role);
      if (!valid_entity_id(b.entity_id)) {
        out.error(role_path, "not a Home Assistant entity id (<domain>.<object_id>)");
      }
      if (b.role == Role::Moisture && !soil_kind) {
        out.error(role_path, "only soil and plant pins bind moisture");
      }
    }
    if (pin.photo) {
      if (pin.kind != PinKind::Plant) {
        out.error(base + "/photo", "only plant pins carry a photo");
      } else if (!valid_photo(*pin.photo)) {
        out.error(base + "/photo", "photo id must match [a-z0-9][a-z0-9_-]{0,47}");
      }
    }

    const Room* room = find_room(config, pin.room_id);
    if (room == nullptr) {
      out.error(base + "/room", "unknown room '" + pin.room_id + "'");
    } else if (position_ok && room->polygon.size() >= kMinVertices &&
               !point_in_polygon(room->polygon, pin.position)) {
      out.warning(base + "/position", "pin lies outside its room's polygon");
    }

    if (find_binding(pin, primary_role(pin.kind)) == nullptr) {
      out.error(base + "/entities", "pin is missing the role its kind requires");
    }
    std::set<Role> roles;
    for (const auto& b : pin.entities) {
      if (!roles.insert(b.role).second) {
        out.error(base + "/entities", "pin binds the same role twice");
        break;
      }
    }

    const bool needs_calibration = soil_kind;
    if (needs_calibration && !pin.calibration) {
      out.error(base + "/calibration", "soil and plant pins need a calibration");
    } else if (!needs_calibration && pin.calibration) {
      out.error(base + "/calibration", "only soil and plant pins carry a calibration");
    } else if (pin.calibration) {
      check_calibration(*pin.calibration, base + "/calibration", out);
    }

    if (pin.stale_after && *pin.stale_after <= 0) {
      out.error(base + "/stale_after_s", "stale_after_s must be positive");
    }
  }
}

bool some_pin_feeds(const Config& config, Layer layer) {
  for (const auto& pin : config.pins) {
    for (const auto& b : pin.entities) {
      if (layer_of(b.role) == layer && feeds_layer(pin.kind, b.role)) return true;
    }
  }
  return false;
}

void check_layers(const Config& config, Issues& out) {
  for (Layer l : kAllLayers) {
    const LayerConfig& lc = config.layer(l);
    const std::string path = std::string("/layers/") + layer_key(l);
    if (!band_finite(lc.band)) {
      out.error(path, "band values must be finite numbers");
    } else if (!band_ordered(lc.band)) {
      out.error(path, "band thresholds are out of order (see schema/README.md, layers)");
    }
    if (lc.enabled && !some_pin_feeds(config, l)) {
      out.warning(path, "layer is enabled but no pin feeds it");
    }
  }
  if (!config.layer(config.default_layer).enabled) {
    out.error("/layers/default", "the default layer must be enabled");
  }
  // The band defaults are Celsius; a Fahrenheit config must set every temperature band field
  // itself (docs/decisions.md D-011). Reported once, however many fields are missing.
  if (config.temperature_unit == TemperatureUnit::Fahrenheit &&
      config.layer(Layer::Temperature).present != kAllBandFields) {
    out.error("/layers/temperature",
              "temperature_unit F needs every temperature band field set explicitly");
  }
}

void check_scalars(const Config& config, Issues& out) {
  if (config.stale_after <= 0) out.error("/stale_after_s", "stale_after_s must be positive");
  if (config.heat.grid_cols < 2 || config.heat.grid_cols > kMaxGridSide) {
    out.error("/heat/grid_cols", "grid_cols must be 2..256");
  }
  if (config.heat.grid_rows < 2 || config.heat.grid_rows > kMaxGridSide) {
    out.error("/heat/grid_rows", "grid_rows must be 2..256");
  }
  if (!(config.heat.idw_power > 0.0 && config.heat.idw_power <= kMaxIdwPower)) {
    out.error("/heat/idw_power", "idw_power must be greater than 0 and at most 8");
  }
  if (config.hud.max_items < 1 || config.hud.max_items > kMaxHudItems) {
    out.error("/hud/max_items", "max_items must be 1..20");
  }
}

}  // namespace

std::vector<ConfigIssue> validate(const Config& config) {
  Issues out;
  if (config.version != kSchemaVersion) out.error("/version", "unsupported schema version");
  check_canvas(config.canvas, out);
  check_rooms(config, out);
  check_pins(config, out);
  check_layers(config, out);
  check_scalars(config, out);
  return out.take();
}

}  // namespace hearthmap
