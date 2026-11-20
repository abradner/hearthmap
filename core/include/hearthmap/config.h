// config.h — plain structs mirroring schema/hearthmap.schema.json, plus the ConfigSource seam.
//
// Core never parses JSON. A Config arrives fully populated from a ConfigSource:
//   - on the device, from static tables the ESPHome component generates at build time;
//   - on the host, from a JSON parser that lives in host/ (not in core).
// Every source applies the schema's documented defaults before handing the Config over, so
// every field below holds a concrete value: core never re-derives a default (docs/decisions.md
// D-007). Field-by-field meaning is in schema/README.md; this header states only what the
// schema cannot (units in core, invariants validate() enforces).
//
// No exceptions are thrown anywhere in core (docs/decisions.md D-002): errors are values.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hearthmap/geometry.h"

namespace hearthmap {

/// Seconds since the Unix epoch, UTC. Core never reads a clock: every function that needs
/// "now" takes one of these.
using Timestamp = std::int64_t;

/// A duration in whole seconds.
using Seconds = std::int64_t;

/// The schema version this header mirrors. A ConfigSource refuses any other version.
inline constexpr int kSchemaVersion = 1;

// ---------------------------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------------------------

enum class Orientation : std::uint8_t { Landscape, Portrait };

struct Canvas {
  std::int32_t width = 1280;                         ///< Canvas pixels after rotation.
  std::int32_t height = 800;                         ///< Canvas pixels after rotation.
  Orientation orientation = Orientation::Landscape;  ///< Landscape requires width > height.
  CanvasRect map_area{0, 0, 960, 800};  ///< Where map space is drawn; inside the canvas.
};

// ---------------------------------------------------------------------------------------------
// Rooms and pins
// ---------------------------------------------------------------------------------------------

struct Room {
  std::string id;    ///< Unique among rooms; [a-z][a-z0-9_]*.
  std::string name;  ///< Display name.
  Polygon polygon;   ///< Map space, >= 3 vertices, simple.
  /// Outdoor rooms (a balcony) are never compared against indoor comfort bands and never
  /// exchange heat-field influence with indoor rooms (docs/decisions.md D-006).
  bool outdoor = false;
};

enum class PinKind : std::uint8_t { Temperature, Humidity, Soil, AirQuality, Plant };

/// The role an entity plays on a pin. A pin binds each role at most once.
enum class Role : std::uint8_t {
  Temperature,     ///< degrees, in the config's temperature unit; feeds Layer::Temperature
  Humidity,        ///< %RH; feeds Layer::Humidity
  Moisture,        ///< raw soil sensor units; feeds Layer::Soil after calibration (as thirst)
  Co2,             ///< ppm; feeds Layer::Air
  Pm25,            ///< ug/m3; display only in v1
  Voc,             ///< sensor-specific index; display only in v1
  Battery,         ///< %; display only
  PredictedDryAt,  ///< reserved for a future watering-time model; ignored by core in v1
};

inline constexpr std::size_t kRoleCount = 8;

struct EntityBinding {
  Role role = Role::Temperature;
  std::string entity_id;  ///< A Home Assistant entity id, e.g. "sensor.example_living_temp".
};

/// Per-sensor calibration for soil and plant pins. `dry` and `wet` are in the sensor's raw
/// units (whatever HA reports: often %, sometimes an ADC count). `wet` may be numerically
/// below `dry` (capacitive probes read lower when wet): that is an inverted range and is fully
/// supported. A zero-width range is rejected by validate(); calibration.h defines what core
/// does if it meets one anyway.
struct Calibration {
  double dry = 0.0;
  double wet = 100.0;
  /// Optional happy range, in the same raw units as dry/wet. `happy_min` is the driest reading
  /// the plant is happy at and `happy_max` the wettest, so on an inverted range happy_min is
  /// numerically the larger. Drier than happy_min the plant is thirsty sooner than the layer
  /// default says; wetter than happy_max it is too wet. See calibration.h,
  /// effective_soil_band().
  std::optional<double> happy_min;
  std::optional<double> happy_max;
};

struct Pin {
  std::string id;  ///< Unique among pins; [a-z][a-z0-9_]*.
  PinKind kind = PinKind::Temperature;
  std::string name;     ///< Display name (the plant's name for plant pins).
  MapPoint position;    ///< Map space, 0..1.
  std::string room_id;  ///< Must name a Room. The position should lie inside that room's
                        ///< polygon; validate() warns (not errors) when it does not.
  std::vector<EntityBinding> entities;  ///< At least the kind's required role (primary_role()).
  std::optional<std::string> photo;     ///< Photo id; resolved by the component/editor, not core.
  std::optional<Calibration> calibration;  ///< Required for Soil and Plant, absent otherwise.
  std::optional<Seconds> stale_after;      ///< Overrides Config::stale_after for this pin.
};

/// The role a pin of `kind` must bind; its reading decides the pin's own severity.
constexpr Role primary_role(PinKind kind) {
  switch (kind) {
    case PinKind::Temperature: return Role::Temperature;
    case PinKind::Humidity: return Role::Humidity;
    case PinKind::Soil: return Role::Moisture;
    case PinKind::AirQuality: return Role::Co2;
    case PinKind::Plant: return Role::Moisture;
  }
  return Role::Temperature;  // unreachable for valid enum values
}

/// The binding for `role` on `pin`, or nullptr when the pin does not bind it.
inline const EntityBinding* find_binding(const Pin& pin, Role role) {
  for (const auto& b : pin.entities) {
    if (b.role == role) return &b;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Layers, bands, thresholds
// ---------------------------------------------------------------------------------------------

/// One heat layer per measured quantity. The UI shows one layer at a time (a tab each).
enum class Layer : std::uint8_t { Temperature, Humidity, Soil, Air };

inline constexpr std::size_t kLayerCount = 4;

constexpr std::size_t index_of(Layer layer) {
  return static_cast<std::size_t>(layer);
}

/// The layer a role feeds, if any. Whether a given pin's binding actually feeds it also
/// depends on the pin (see feeds_layer()).
constexpr std::optional<Layer> layer_of(Role role) {
  switch (role) {
    case Role::Temperature: return Layer::Temperature;
    case Role::Humidity: return Layer::Humidity;
    case Role::Moisture: return Layer::Soil;
    case Role::Co2: return Layer::Air;
    default: return std::nullopt;
  }
}

/// Whether `pin`'s binding for `role` contributes to `layer_of(role)` (heat field, pulsing,
/// pin colour on that tab). Soil and plant pins' temperature/humidity roles are display-only:
/// a probe in a pot measures the pot, not the room (docs/decisions.md D-004).
constexpr bool feeds_layer(PinKind kind, Role role) {
  if (!layer_of(role).has_value()) return false;
  if ((kind == PinKind::Soil || kind == PinKind::Plant) &&
      (role == Role::Temperature || role == Role::Humidity)) {
    return false;
  }
  return true;
}

/// Comfort band and thresholds for one layer, in the layer's value domain:
///   Temperature: degrees in the config's unit; Humidity: %RH; Soil: thirst 0..1 (0 = just
///   watered, 1 = bone dry; calibration.h); Air: CO2 ppm.
///
/// Severity (severity.h, classify()) uses the thresholds; colour (colour_scale.h) uses the
/// comfort band and the scale extent. A missing threshold means "no threshold on that side".
/// validate() enforces, for every threshold present:
///   scale_min <= danger_below <= warn_below <= comfort_min <= comfort_max
///             <= warn_above <= danger_above <= scale_max, and scale_min < scale_max.
struct Band {
  double comfort_min = 0.0;
  double comfort_max = 0.0;
  std::optional<double> warn_below;
  std::optional<double> warn_above;
  std::optional<double> danger_below;
  std::optional<double> danger_above;
  double scale_min = 0.0;  ///< Value at which the cold colour saturates.
  double scale_max = 0.0;  ///< Value at which the hot colour saturates.
};

struct LayerConfig {
  bool enabled = true;  ///< Disabled layers get no tab, no heat field and no pulsing.
  Band band;
};

enum class TemperatureUnit : std::uint8_t { Celsius, Fahrenheit };

/// The schema's documented default band for `layer` (Celsius for temperature). These are the
/// values every ConfigSource fills in for an omitted field; schema/README.md lists the same
/// numbers and the CI schema check asserts the two agree (stated, not yet enforced: PR 2).
constexpr Band default_band(Layer layer) {
  switch (layer) {
    case Layer::Temperature: return Band{19.0, 24.0, 16.0, 27.0, 12.0, 32.0, 10.0, 34.0};
    case Layer::Humidity: return Band{40.0, 60.0, 30.0, 65.0, 20.0, 75.0, 15.0, 90.0};
    case Layer::Soil: return Band{0.1, 0.6, std::nullopt, 0.7, std::nullopt, 0.9, 0.0, 1.0};
    case Layer::Air:
      return Band{400.0, 800.0, std::nullopt, 1200.0, std::nullopt, 2000.0, 400.0, 2500.0};
  }
  return Band{};
}

// ---------------------------------------------------------------------------------------------
// Heat field, HUD, top level
// ---------------------------------------------------------------------------------------------

struct HeatConfig {
  std::int32_t grid_cols = 48;  ///< Cells across the map area; >= 2.
  std::int32_t grid_rows = 40;  ///< Cells down the map area; >= 2.
  double idw_power = 2.0;       ///< Inverse-distance weighting exponent; > 0.
};

struct HudConfig {
  std::int32_t max_items = 6;  ///< Plants listed before "+N more"; >= 1.
};

struct Config {
  int version = kSchemaVersion;
  Canvas canvas;
  TemperatureUnit temperature_unit = TemperatureUnit::Celsius;
  std::vector<Room> rooms;  ///< Order matters: first room wins where polygons overlap.
  std::vector<Pin> pins;    ///< Order is the tie-breaker everywhere a tie can occur.
  /// Indexed by index_of(Layer). Defaults to every layer enabled with default_band().
  std::array<LayerConfig, kLayerCount> layers{{
      {true, default_band(Layer::Temperature)},
      {true, default_band(Layer::Humidity)},
      {true, default_band(Layer::Soil)},
      {true, default_band(Layer::Air)},
  }};
  Layer default_layer = Layer::Temperature;  ///< Must be enabled.
  Seconds stale_after = 10800;               ///< Global staleness timeout; > 0.
  HeatConfig heat;
  HudConfig hud;

  const LayerConfig& layer(Layer l) const { return layers[index_of(l)]; }
};

// ---------------------------------------------------------------------------------------------
// Validation and the source seam
// ---------------------------------------------------------------------------------------------

enum class IssueSeverity : std::uint8_t { Error, Warning };

/// One problem found in a Config. `path` is a JSON-pointer-style location matching the schema
/// ("/pins/3/room"), so host, editor and codegen can report the same place.
struct ConfigIssue {
  IssueSeverity severity = IssueSeverity::Error;
  std::string path;
  std::string message;
};

/// Semantic checks the JSON Schema cannot express. Errors make a Config unusable; warnings do
/// not. Errors: duplicate room or pin ids; a pin naming an unknown room; a polygon with fewer
/// than 3 vertices or self-intersecting; a pin missing its primary role, binding a role twice,
/// or (Soil/Plant) missing its calibration, or (other kinds) carrying one; a zero-width
/// calibration (|wet - dry| < 1e-9); a happy_min/happy_max outside the dry..wet span, or
/// happy_min wetter than happy_max; band ordering violated (see Band); a landscape canvas with
/// width <= height or portrait with height <= width; a map area outside the canvas; default_layer
/// disabled; non-positive stale_after, grid size < 2, idw_power <= 0, hud.max_items < 1. Warnings:
/// a pin whose position is outside its room's polygon; an enabled layer no pin feeds. Implemented
/// in core (PR 2).
std::vector<ConfigIssue> validate(const Config& config);

/// True when `issues` contains no Error.
inline bool usable(const std::vector<ConfigIssue>& issues) {
  for (const auto& i : issues) {
    if (i.severity == IssueSeverity::Error) return false;
  }
  return true;
}

/// The outcome of loading a config: a Config when loading succeeded (it may still carry
/// warnings), or no Config and at least one Error.
struct ConfigLoadResult {
  std::optional<Config> config;
  std::vector<ConfigIssue> issues;
};

/// The seam between where a config comes from and the code that uses it. Implementations:
/// host/ parses JSON; components/hearthmap returns generated static tables; tests build one
/// in code. An implementation must apply schema defaults, check `version == kSchemaVersion`,
/// and run validate(), returning no Config if validate() reports an Error. It must also refuse
/// (with an Error at "/layers/temperature") a config with temperature_unit "F" that omits any
/// temperature band field, because the defaults are Celsius (docs/decisions.md D-011).
class ConfigSource {
 public:
  virtual ~ConfigSource() = default;
  virtual ConfigLoadResult load() const = 0;
};

}  // namespace hearthmap
