// config.h — plain structs mirroring schema/hearthmap.schema.json, plus the ConfigSource seam.
//
// Core never parses JSON. A Config arrives fully populated from a ConfigSource:
//   - on the device, from static tables the ESPHome component generates at build time;
//   - on the host, from a JSON parser that lives in host/ (not in core).
// Every source applies the schema's documented defaults before handing the Config over, so
// every field below holds a concrete value: core never re-derives a default (docs/decisions.md
// D-007). Every source also runs validate(), which re-checks every schema range: nothing in core
// relies on JSON Schema having run (D-017). Field-by-field meaning is in schema/README.md; this
// header states only what the schema cannot (units in core, invariants validate() enforces).
//
// No exceptions are thrown anywhere in core (docs/decisions.md D-002): errors are values.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
  /// Where map space is drawn; inside the canvas. When the config omits it, the source sets
  /// the whole canvas: {0, 0, width, height} (docs/decisions.md D-023).
  CanvasRect map_area{0, 0, 1280, 800};
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
  Moisture,        ///< raw soil sensor units; feeds Layer::Soil after calibration (as thirst).
                   ///< Bindable on Soil and Plant pins only (validate() errors otherwise, D-018).
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
/// does if it meets one anyway. "Between dry and wet" is inclusive everywhere in this contract:
/// a happy edge exactly equal to `dry` or `wet` is inside the span.
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
/// a probe in a pot measures the pot, not the room (docs/decisions.md D-004). Moisture feeds
/// only on Soil and Plant pins, the only kinds that carry a calibration (D-018); validate()
/// rejects it elsewhere, so the `false` here is defence, not policy.
constexpr bool feeds_layer(PinKind kind, Role role) {
  if (!layer_of(role).has_value()) return false;
  const bool soil_like = kind == PinKind::Soil || kind == PinKind::Plant;
  if (soil_like && (role == Role::Temperature || role == Role::Humidity)) return false;
  if (!soil_like && role == Role::Moisture) return false;
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

/// The eight fields of a Band, for recording which ones a config set explicitly.
enum class BandField : std::uint8_t {
  ComfortMin,
  ComfortMax,
  WarnBelow,
  WarnAbove,
  DangerBelow,
  DangerAbove,
  ScaleMin,
  ScaleMax,
};

inline constexpr std::size_t kBandFieldCount = 8;

/// The bit for `field` in LayerConfig::present.
constexpr std::uint8_t band_field_bit(BandField field) {
  return static_cast<std::uint8_t>(1U << static_cast<unsigned>(field));
}

/// Every BandField bit set.
inline constexpr std::uint8_t kAllBandFields = 0xFF;

struct LayerConfig {
  /// Disabled layers get no tab, no heat field and no pulsing, and the roles that feed them are
  /// not judged (severity.h, Assessment::severity; docs/decisions.md D-020).
  bool enabled = true;
  Band band;
  /// Which band fields the config itself set (band_field_bit() per field), as opposed to
  /// defaults the source filled in. A field set to null in JSON counts as set. The source only
  /// records presence; validate() decides what it means (the Fahrenheit rule, D-011).
  std::uint8_t present = 0;
};

enum class TemperatureUnit : std::uint8_t { Celsius, Fahrenheit };

/// The schema's documented default band for `layer` (Celsius for temperature). These are the
/// values every ConfigSource fills in for an omitted field; schema/README.md lists the same
/// numbers, and ctest's core.defaults_match_docs fails when the two disagree.
constexpr Band default_band(Layer layer) {
  switch (layer) {
    case Layer::Temperature: return Band{19.0, 24.0, 16.0, 27.0, 12.0, 32.0, 10.0, 34.0};
    case Layer::Humidity: return Band{40.0, 60.0, 30.0, 65.0, 20.0, 75.0, 15.0, 90.0};
    case Layer::Soil: return Band{0.1, 0.6, std::nullopt, 0.7, std::nullopt, 0.9, 0.0, 1.0};
    case Layer::Air:
      return Band{400.0, 800.0, std::nullopt, 1200.0, std::nullopt, 2000.0, 350.0, 2500.0};
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
      {true, default_band(Layer::Temperature), 0},
      {true, default_band(Layer::Humidity), 0},
      {true, default_band(Layer::Soil), 0},
      {true, default_band(Layer::Air), 0},
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

/// Every check on a Config: every range and pattern the JSON Schema states, re-checked here
/// (no source may rely on the schema having run, D-017), plus the semantic rules a schema
/// cannot express. Errors make a Config unusable; warnings do not.
///
/// Errors, schema ranges (schema/hearthmap.schema.json is the source of each number):
///   version != kSchemaVersion; canvas width or height outside 1..4096; map_area x or y < 0,
///   width or height < 1, or not inside the canvas; no rooms, more than 64 rooms, more than 256
///   pins; a room or pin id not matching ^[a-z][a-z0-9_]*$ or longer than 48 characters; a room
///   or pin name that is not valid UTF-8, is empty, or is longer than 48 characters (counted in
///   Unicode code points, as the schema's maxLength counts them); a polygon with fewer
///   than 3 or more than 64 vertices; a polygon vertex or pin position with x or y outside 0..1
///   (or non-finite); an entity id not matching ^[a-z0-9_]+\.[a-z0-9_]+$; a photo on a pin
///   that is not a Plant, or a photo id not matching ^[a-z0-9][a-z0-9_-]{0,47}$; stale_after or
///   a pin's stale_after < 1; grid_cols or grid_rows outside 2..256; idw_power not in (0, 8]
///   (or non-finite); hud.max_items outside 1..20; a non-finite band value.
/// Errors, semantic rules:
///   duplicate room or pin ids; a pin naming an unknown room; a polygon that is not simple
///   (edges cross or overlap, a zero-length edge, or zero area); a pin
///   missing its primary role, binding a role twice, binding Moisture when it is not Soil or
///   Plant (D-018), or (Soil/Plant) missing its calibration, or (other kinds) carrying one; a
///   calibration whose span is unusable (calibration_span_usable() false, calibration.h:
///   zero-width, non-finite, or overflowing); a happy_min or happy_max that is non-finite or
///   outside the dry..wet span (endpoints count as inside), or happy_min wetter than
///   happy_max; band ordering violated (see Band) on any layer, enabled or not; a landscape
///   canvas with width <= height or portrait with height <= width; default_layer disabled;
///   temperature_unit Fahrenheit while layers[Temperature].present != kAllBandFields, reported
///   once at "/layers/temperature" (the defaults are Celsius, D-011).
/// Warnings: a pin whose position is outside its room's polygon; an enabled layer no pin feeds.
/// Implemented in core (PR 2).
std::vector<ConfigIssue> validate(const Config& config);

/// validate(), one unit at a time, for callers that must not block (the device's boot,
/// docs/decisions.md D-038). Each step() checks one unit: first the version, canvas and room
/// count; then one room per step; then the pin count; then one pin per step; then the layers and
/// the remaining scalars. Stepping until step() returns true gives exactly validate()'s issues, in
/// the same order. `config` must outlive the validator and
/// not change while it runs. A room step does at most O(V^2) segment tests for its own V <= 64
/// vertices (fewer for an invalid one); a pin step is a few checks and one point_in_polygon().
class ConfigValidator {
 public:
  explicit ConfigValidator(const Config& config);
  ~ConfigValidator();
  ConfigValidator(const ConfigValidator&) = delete;
  ConfigValidator& operator=(const ConfigValidator&) = delete;

  /// Check the next unit. Returns done().
  bool step();
  bool done() const;
  /// 3 + rooms + pins.
  std::size_t steps_total() const;
  /// The issues found so far (all of them once done()); leaves the validator's list empty.
  std::vector<ConfigIssue> take_issues();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

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
/// in code. An implementation must apply schema defaults (map_area defaults to the whole
/// canvas), record which band fields the config set (LayerConfig::present), and run
/// validate(), returning no Config if validate() reports an Error. It must not re-implement
/// any rule validate() owns, the Fahrenheit rule included (D-011, D-017): enforcement lives in
/// one place. Errors only a parser can see (malformed JSON, an unknown field, a wrong type)
/// are the source's own.
class ConfigSource {
 public:
  virtual ~ConfigSource() = default;
  virtual ConfigLoadResult load() const = 0;
};

}  // namespace hearthmap
