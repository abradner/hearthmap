"""Build-time code generation for the hearthmap ESPHome component.

Pure Python with no ESPHome import, so it is unit-tested on its own
(components/hearthmap/tests/). The component's __init__.py is the only caller on a real build.

What it does:
  - checks a config document against schema/hearthmap.schema.json (check_schema; ESPHome's image
    has no jsonschema package, so this is a small validator for exactly the keywords that schema
    uses, refusing any keyword it does not implement rather than skipping it);
  - turns the document into C++ that builds a hearthmap::Config and runs core's validate()
    (emit_config_source). Scalar defaults come from the schema's own "default" keywords; band
    defaults come from core's default_band() at run time, so neither is copied here
    (docs/decisions.md D-007). LayerConfig::present gets a bit for exactly the band fields the
    document wrote, null included (D-011);
  - converts plant photos to RGB565 image data for LVGL (emit_photos);
  - maps the core and ui_lvgl sources into the firmware build tree (library_files).

None of the semantic rules core's validate() owns are re-implemented here (D-017): the firmware
runs validate() at boot and refuses a config it rejects.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

# ---------------------------------------------------------------------------------------------
# Constants that mirror core's enums (core/include/hearthmap/config.h). Order matters: it is the
# enum order, and bindings are emitted in it (D-019).
# ---------------------------------------------------------------------------------------------

ROLES = (
    ("temperature", "Temperature"),
    ("humidity", "Humidity"),
    ("moisture", "Moisture"),
    ("co2", "Co2"),
    ("pm25", "Pm25"),
    ("voc", "Voc"),
    ("battery", "Battery"),
    ("predicted_dry_at", "PredictedDryAt"),
)
# Roles core ignores in v1 (config.h, Role::PredictedDryAt). Bound in the Config, never
# subscribed: there is nothing to show for them yet.
UNSUBSCRIBED_ROLES = frozenset({"predicted_dry_at"})

KINDS = {
    "temperature": "Temperature",
    "humidity": "Humidity",
    "soil": "Soil",
    "air_quality": "AirQuality",
    "plant": "Plant",
}
LAYERS = (
    ("temperature", "Temperature"),
    ("humidity", "Humidity"),
    ("soil", "Soil"),
    ("air", "Air"),
)
# BandField enum order: bit i of LayerConfig::present is field i.
BAND_FIELDS = (
    ("comfort_min", "comfort_min", False),
    ("comfort_max", "comfort_max", False),
    ("warn_below", "warn_below", True),
    ("warn_above", "warn_above", True),
    ("danger_below", "danger_below", True),
    ("danger_above", "danger_above", True),
    ("scale_min", "scale_min", False),
    ("scale_max", "scale_max", False),
)

# Plant photos are converted to every disc diameter ui_lvgl draws them at (ui_lvgl/src/screen.cpp:
# kPlantRadius 22, kHudDiscRadius 26, kOverlayDiscRadius 64), so the panel never scales an RGB565
# image: its LVGL build has no software transform for RGB565 (docs/decisions.md D-039). A test
# reads those constants from screen.cpp and fails if they drift from these.
PHOTO_SIZES = (44, 52, 128)
PHOTO_SIZE = max(PHOTO_SIZES)  # the size plant_photo() returns
# Background a transparent photo is flattened onto: the plant disc fill from the UI spec.
PHOTO_BACKGROUND = (0x16, 0x26, 0x1C)


class ConfigError(Exception):
    """A config the generator refuses. `path` is a JSON pointer, like core's ConfigIssue."""

    def __init__(self, path: str, message: str):
        super().__init__(f"{path or '/'}: {message}")
        self.path = path
        self.message = message


# ---------------------------------------------------------------------------------------------
# Schema check
# ---------------------------------------------------------------------------------------------

_ANNOTATIONS = frozenset({"description", "default", "title", "$schema", "$id", "$defs", "examples"})
_SUPPORTED = _ANNOTATIONS | frozenset(
    {
        "type", "$ref", "required", "properties", "additionalProperties", "minimum", "maximum",
        "exclusiveMinimum", "const", "enum", "not", "if", "then", "else", "allOf", "anyOf",
        "items", "prefixItems", "minItems", "maxItems", "pattern", "minLength", "maxLength",
        "minProperties",
    }
)


class SchemaUnsupported(Exception):
    """The schema uses a keyword this checker does not implement. Fails loudly by design: a
    validator that skipped unknown keywords would pass configs the schema rejects."""


def _is_type(value: Any, name: str) -> bool:
    if name == "null":
        return value is None
    if name == "boolean":
        return isinstance(value, bool)
    if name == "object":
        return isinstance(value, dict)
    if name == "array":
        return isinstance(value, list)
    if name == "string":
        return isinstance(value, str)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if name == "integer":
        if isinstance(value, bool):
            return False
        if isinstance(value, int):
            return True
        return isinstance(value, float) and math.isfinite(value) and value.is_integer()
    raise SchemaUnsupported(f"type {name!r}")


def _json_equal(a: Any, b: Any) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    return a == b


def _pointer_token(key: str) -> str:
    """An object key as a JSON Pointer token (RFC 6901): "~" -> "~0", "/" -> "~1"."""
    return key.replace("~", "~0").replace("/", "~1")


def _check_keywords(schema: Any, where: str = "#") -> None:
    """Raise SchemaUnsupported for any keyword the checker does not implement, anywhere in the
    schema, whether or not a given config reaches it. Walks schema-valued positions only, so keys
    inside annotations ("description", "default", ...) and property names are not keywords."""
    if isinstance(schema, bool):
        return
    if not isinstance(schema, dict):
        raise SchemaUnsupported(f"{where}: not a schema")
    unknown = set(schema) - _SUPPORTED
    if unknown:
        raise SchemaUnsupported(f"{where}: keywords {sorted(unknown)}")
    for key in ("properties", "$defs"):
        for name, sub in schema.get(key, {}).items():
            _check_keywords(sub, f"{where}/{key}/{_pointer_token(name)}")
    for key in ("items", "additionalProperties", "not", "if", "then", "else"):
        if key in schema:
            _check_keywords(schema[key], f"{where}/{key}")
    for key in ("allOf", "anyOf", "prefixItems"):
        for i, sub in enumerate(schema.get(key, [])):
            _check_keywords(sub, f"{where}/{key}/{i}")


class _Checker:
    def __init__(self, root: dict):
        self.root = root

    def resolve(self, ref: str) -> dict:
        if not ref.startswith("#/"):
            raise SchemaUnsupported(f"non-local $ref {ref!r}")
        node: Any = self.root
        for part in ref[2:].split("/"):
            node = node[part.replace("~1", "/").replace("~0", "~")]
        return node

    def errors(self, schema: Any, value: Any, path: str) -> Iterator[tuple[str, str]]:
        if schema is True:
            return
        if schema is False:
            yield path, "not allowed here"
            return
        unknown = set(schema) - _SUPPORTED
        if unknown:
            raise SchemaUnsupported(f"keywords {sorted(unknown)}")

        if "$ref" in schema:
            yield from self.errors(self.resolve(schema["$ref"]), value, path)
        if "type" in schema:
            types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(_is_type(value, t) for t in types):
                yield path, f"expected {' or '.join(types)}"
                return
        if "const" in schema and not _json_equal(value, schema["const"]):
            yield path, f"must be {json.dumps(schema['const'])}"
        if "enum" in schema and not any(_json_equal(value, e) for e in schema["enum"]):
            yield path, f"must be one of {json.dumps(schema['enum'])}"

        if _is_type(value, "number"):
            if "minimum" in schema and value < schema["minimum"]:
                yield path, f"must be >= {schema['minimum']}"
            if "maximum" in schema and value > schema["maximum"]:
                yield path, f"must be <= {schema['maximum']}"
            if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
                yield path, f"must be > {schema['exclusiveMinimum']}"
        if isinstance(value, str):
            if "minLength" in schema and len(value) < schema["minLength"]:
                yield path, f"must be at least {schema['minLength']} characters"
            if "maxLength" in schema and len(value) > schema["maxLength"]:
                yield path, f"must be at most {schema['maxLength']} characters"
            if "pattern" in schema and not re.search(schema["pattern"], value):
                yield path, f"must match {schema['pattern']}"
        if isinstance(value, list):
            if "minItems" in schema and len(value) < schema["minItems"]:
                yield path, f"needs at least {schema['minItems']} items"
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                yield path, f"allows at most {schema['maxItems']} items"
            prefix = schema.get("prefixItems", [])
            for i, sub in enumerate(prefix[: len(value)]):
                yield from self.errors(sub, value[i], f"{path}/{i}")
            if "items" in schema:
                for i in range(len(prefix), len(value)):
                    yield from self.errors(schema["items"], value[i], f"{path}/{i}")
        if isinstance(value, dict):
            for key in schema.get("required", []):
                if key not in value:
                    yield path, f"missing required field {key!r}"
            if "minProperties" in schema and len(value) < schema["minProperties"]:
                yield path, f"needs at least {schema['minProperties']} fields"
            props = schema.get("properties", {})
            for key, sub in props.items():
                if key in value:
                    yield from self.errors(sub, value[key], f"{path}/{_pointer_token(key)}")
            if "additionalProperties" in schema:
                for key in value:
                    if key not in props:
                        yield from (
                            (p, f"unknown field {key!r}" if m == "not allowed here" else m)
                            for p, m in self.errors(
                                schema["additionalProperties"], value[key], f"{path}/{_pointer_token(key)}"
                            )
                        )

        for sub in schema.get("allOf", []):
            yield from self.errors(sub, value, path)
        if "anyOf" in schema:
            if not any(self.valid(sub, value) for sub in schema["anyOf"]):
                yield path, "matches none of the allowed shapes"
        if "not" in schema and self.valid(schema["not"], value):
            yield path, "has a combination of fields that is not allowed"
        if "if" in schema:
            branch = "then" if self.valid(schema["if"], value) else "else"
            if branch in schema:
                yield from self.errors(schema[branch], value, path)

    def valid(self, schema: Any, value: Any) -> bool:
        return next(iter(self.errors(schema, value, "")), None) is None


def check_schema(schema: dict, document: Any) -> list[tuple[str, str]]:
    """Every schema violation in `document`, as (json pointer, message). Empty means valid.
    Raises SchemaUnsupported first if the schema uses a keyword this checker lacks."""
    _check_keywords(schema)
    return list(_Checker(schema).errors(schema, document, ""))


def schema_defaults(schema: dict) -> dict[str, Any]:
    """The schema's "default" values that the generator applies, by JSON-pointer-like path.
    Read from the schema so the numbers live in one contract, not in this file too."""
    c = _Checker(schema)
    defs = schema["$defs"]

    def default(node: dict) -> Any:
        if "$ref" in node:
            node = c.resolve(node["$ref"])
        return node["default"]

    top = schema["properties"]
    return {
        "temperature_unit": default(top["temperature_unit"]),
        "stale_after_s": default(top["stale_after_s"]),
        "room.outdoor": default(defs["room"]["properties"]["outdoor"]),
        "layer.enabled": default(defs["layer"]["properties"]["enabled"]),
        "layers.default": default(defs["layers"]["properties"]["default"]),
        "hud.max_items": default(defs["hud"]["properties"]["max_items"]),
        "heat.grid_cols": default(defs["heat"]["properties"]["grid_cols"]),
        "heat.grid_rows": default(defs["heat"]["properties"]["grid_rows"]),
        "heat.idw_power": default(defs["heat"]["properties"]["idw_power"]),
    }


# ---------------------------------------------------------------------------------------------
# C++ emission
# ---------------------------------------------------------------------------------------------


def cpp_string(s: str) -> str:
    """A C++ string literal for `s`, UTF-8, every non-printable-ASCII byte escaped in octal (so a
    following hex digit can never extend the escape)."""
    out = ['"']
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= b < 0x7F and ch != "?":
            out.append(ch)
        else:
            out.append(f"\\{b:03o}")
    out.append('"')
    return "".join(out)


def cpp_std_string(s: str) -> str:
    """A std::string expression holding exactly `s`: built with an explicit byte length, so an
    embedded NUL (schema-valid in a display name) is kept rather than ending the string."""
    return f"std::string({cpp_string(s)}, {len(s.encode('utf-8'))})"


def cpp_double(x: float | int) -> str:
    """A C++ double literal that round-trips exactly (repr is the shortest round-trip form)."""
    v = float(x)
    if not math.isfinite(v):
        raise ConfigError("", f"non-finite number {x!r}")
    r = repr(v)
    return r if ("e" in r or "." in r) else r + ".0"


INT32 = (-(2**31), 2**31 - 1)
INT64 = (-(2**63), 2**63 - 1)


def cpp_int(value: Any, path: str, bounds: tuple[int, int] = INT32) -> str:
    """An integer literal for a C++ field of the given range. A mathematically integral number
    spelled as a float (1280.0, 4.8e1) is accepted, as JSON Schema's "integer" does; a fractional
    one, a non-number or a value outside the destination type is a ConfigError at `path`, never
    narrowed (the same rule as the host's JSON source)."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(path, f"expected an integer, got {value!r}")
    if isinstance(value, float):
        if not math.isfinite(value) or not value.is_integer():
            raise ConfigError(path, f"expected an integer, got {value!r}")
        value = int(value)
    lo, hi = bounds
    if not lo <= value <= hi:
        raise ConfigError(path, f"{value} does not fit the field's range {lo}..{hi}")
    return str(value)


GENERATED_HEADER = """\
// Generated by the hearthmap ESPHome component (components/hearthmap). Do not edit.
#pragma once

#include <cstddef>

#include "hearthmap/config.h"

namespace hearthmap_gen {

/// The config compiled into this firmware, behind core's ConfigSource seam: builds a Config from
/// static tables (schema defaults applied, LayerConfig::present recorded) and runs validate().
class GeneratedConfigSource final : public ::hearthmap::ConfigSource {
 public:
  /// The tables as a Config with schema defaults applied and present bits recorded, not yet
  /// validated: for callers that run validate() in steps (core's ConfigValidator, D-038). Use it
  /// only with a validation that refuses on Error, as load() does.
  ::hearthmap::Config build() const;
  /// build() a unit at a time, for callers that must not block: build_step(c, k) for every k in
  /// 0 .. build_steps() - 1, in order, on a default-constructed Config gives build(). Units: a
  /// head (canvas, reserves), one per room, one per pin, a tail (layers and scalars).
  std::size_t build_steps() const;
  void build_step(::hearthmap::Config &c, std::size_t k) const;
  /// build(), then validate(); no Config when it reports an Error.
  ::hearthmap::ConfigLoadResult load() const override;
};

/// One Home Assistant entity the firmware subscribes to.
struct SubscribedEntity {
  const char *entity_id;  ///< Static storage: safe to hand to the API's zero-copy subscribe.
};

/// Every distinct entity id bound by a pin, in first-use order, except roles core ignores in v1.
extern const SubscribedEntity kSubscribedEntities[];
extern const std::size_t kSubscribedEntityCount;

/// File name of the config the tables came from, for logs.
extern const char *const kConfigSourceName;

}  // namespace hearthmap_gen
"""


@dataclass(frozen=True)
class GeneratedConfig:
    header: str
    source: str
    entity_ids: tuple[str, ...]


def _p(path: str, key: Any) -> str:
    return f"{path}/{key}"


def _check_strings(node: Any, path: str) -> None:
    """Every string in the document must be valid Unicode: JSON allows an escaped lone surrogate
    ("\\ud800"), which Python keeps and UTF-8 cannot encode. Report it at its path as a
    ConfigError instead of letting the encoder raise during generation."""
    if isinstance(node, str):
        try:
            node.encode("utf-8")
        except UnicodeEncodeError:
            raise ConfigError(path, "contains an invalid character (a lone UTF-16 surrogate)") from None
    elif isinstance(node, dict):
        for k, v in node.items():
            _check_strings(k, path)
            _check_strings(v, f"{path}/{_pointer_token(k)}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            _check_strings(v, f"{path}/{i}")


def emit_config_source(document: dict, schema: dict, source_name: str) -> GeneratedConfig:
    """C++ for a document that passed check_schema(). Raises ConfigError only for what a
    schema-valid document can still get wrong in a way C++ cannot represent (a non-finite
    number, an integer outside its field's type); every semantic rule is left to validate() on
    the device."""
    _check_strings(document, "")
    d = schema_defaults(schema)
    lines: list[str] = []
    w = lines.append

    w("// Generated by the hearthmap ESPHome component (components/hearthmap). Do not edit.")
    w('#include "hearthmap_gen/generated_config.h"')
    w("")
    w("#include <utility>")
    w("")
    w("namespace hearthmap_gen {")
    w("")
    w(f"const char *const kConfigSourceName = {cpp_string(source_name)};")
    w("")
    # build() is split into one function per room and per pin plus a head and a tail, so the
    # device can build the Config a unit at a time (build_step(), D-038).
    steps: list[str] = ["build_head"]
    w("namespace {")
    w("")
    w("using namespace ::hearthmap;")
    w("")
    w("void build_head(Config &c) {")
    w(f"  c.version = {cpp_int(document['version'], '/version')};")

    canvas = document["canvas"]
    width = int(cpp_int(canvas["width"], "/canvas/width"))
    height = int(cpp_int(canvas["height"], "/canvas/height"))
    w(f"  c.canvas.width = {width};")
    w(f"  c.canvas.height = {height};")
    orient = "Landscape" if canvas["orientation"] == "landscape" else "Portrait"
    w(f"  c.canvas.orientation = Orientation::{orient};")
    # R14 / D-023: an omitted map_area is the whole canvas.
    ma = canvas.get("map_area", {"x": 0, "y": 0, "width": width, "height": height})
    rect = ", ".join(cpp_int(ma[k], f"/canvas/map_area/{k}") for k in ("x", "y", "width", "height"))
    w(f"  c.canvas.map_area = CanvasRect{{{rect}}};")
    unit = document.get("temperature_unit", d["temperature_unit"])
    w(f"  c.temperature_unit = TemperatureUnit::{'Fahrenheit' if unit == 'F' else 'Celsius'};")

    rooms = document["rooms"]
    pins = document["pins"]
    w(f"  c.rooms.reserve({len(rooms)});")
    w(f"  c.pins.reserve({len(pins)});")
    w("}")
    w("")
    for i, room in enumerate(rooms):
        pts = ", ".join(
            f"MapPoint{{{cpp_double(x)}, {cpp_double(y)}}}" for x, y in room["polygon"]
        )
        outdoor = bool(room.get("outdoor", d["room.outdoor"]))
        steps.append(f"build_room_{i}")
        w(f"void build_room_{i}(Config &c) {{")
        w("    Room r;")
        w(f"    r.id = {cpp_std_string(room['id'])};")
        w(f"    r.name = {cpp_std_string(room['name'])};")
        w(f"    r.polygon = Polygon{{{pts}}};")
        w(f"    r.outdoor = {'true' if outdoor else 'false'};")
        w("    c.rooms.push_back(std::move(r));")
        w("}")
        w("")

    entity_ids: list[str] = []
    for i, pin in enumerate(pins):
        path = f"/pins/{i}"
        steps.append(f"build_pin_{i}")
        w(f"void build_pin_{i}(Config &c) {{")
        w("    Pin p;")
        w(f"    p.id = {cpp_std_string(pin['id'])};")
        w(f"    p.kind = PinKind::{KINDS[pin['kind']]};")
        w(f"    p.name = {cpp_std_string(pin['name'])};")
        x, y = pin["position"]
        w(f"    p.position = MapPoint{{{cpp_double(x)}, {cpp_double(y)}}};")
        w(f"    p.room_id = {cpp_std_string(pin['room'])};")
        ents = pin["entities"]
        bound = [(key, enum) for key, enum in ROLES if key in ents]
        w(f"    p.entities.reserve({len(bound)});")
        for key, enum in bound:  # Role enum order, whatever the JSON key order (D-019)
            eid = ents[key]
            w(f"    p.entities.push_back(EntityBinding{{Role::{enum}, {cpp_std_string(eid)}}});")
            if key not in UNSUBSCRIBED_ROLES and eid not in entity_ids:
                entity_ids.append(eid)
        if "photo" in pin:
            w(f"    p.photo = {cpp_std_string(pin['photo'])};")
        if "calibration" in pin:
            cal = pin["calibration"]
            w("    Calibration cal;")
            try:
                w(f"    cal.dry = {cpp_double(cal['dry'])};")
                w(f"    cal.wet = {cpp_double(cal['wet'])};")
                for k in ("happy_min", "happy_max"):
                    if k in cal:
                        w(f"    cal.{k} = {cpp_double(cal[k])};")
            except ConfigError as e:
                raise ConfigError(_p(path, "calibration"), e.message) from None
            w("    p.calibration = cal;")
        if "stale_after_s" in pin:
            w(f"    p.stale_after = Seconds{{{cpp_int(pin['stale_after_s'], path + '/stale_after_s', INT64)}}};")
        w("    c.pins.push_back(std::move(p));")
        w("}")
        w("")

    steps.append("build_tail")
    w("void build_tail(Config &c) {")
    layers = document.get("layers", {})
    for key, enum in LAYERS:
        lc = layers.get(key, {})
        enabled = bool(lc.get("enabled", d["layer.enabled"]))
        w("  {")
        w(f"    LayerConfig &l = c.layers[index_of(Layer::{enum})];")
        w(f"    l.enabled = {'true' if enabled else 'false'};")
        # Band defaults come from core (config.h default_band(), D-007); only written fields
        # overwrite them, and only written fields set a present bit (D-011).
        w(f"    l.band = default_band(Layer::{enum});")
        present = 0
        for bit, (json_key, member, nullable) in enumerate(BAND_FIELDS):
            if json_key not in lc:
                continue
            present |= 1 << bit
            v = lc[json_key]
            if v is None:
                # The schema allows null only on thresholds; it removes that threshold.
                w(f"    l.band.{member} = std::nullopt;")
            else:
                try:
                    w(f"    l.band.{member} = {cpp_double(v)};")
                except ConfigError as e:
                    raise ConfigError(f"/layers/{key}/{json_key}", e.message) from None
        w(f"    l.present = 0x{present:02X};")
        w("  }")
    default_layer = layers.get("default", d["layers.default"])
    w(f"  c.default_layer = Layer::{dict(LAYERS)[default_layer]};")
    stale = cpp_int(document.get("stale_after_s", d["stale_after_s"]), "/stale_after_s", INT64)
    w(f"  c.stale_after = Seconds{{{stale}}};")
    heat = document.get("heat", {})
    w(f"  c.heat.grid_cols = {cpp_int(heat.get('grid_cols', d['heat.grid_cols']), '/heat/grid_cols')};")
    w(f"  c.heat.grid_rows = {cpp_int(heat.get('grid_rows', d['heat.grid_rows']), '/heat/grid_rows')};")
    w(f"  c.heat.idw_power = {cpp_double(heat.get('idw_power', d['heat.idw_power']))};")
    hud = document.get("hud", {})
    w(f"  c.hud.max_items = {cpp_int(hud.get('max_items', d['hud.max_items']), '/hud/max_items')};")
    w("}")
    w("")
    w("using BuildFn = void (*)(Config &);")
    w("const BuildFn kBuildSteps[] = {")
    for name in steps:
        w(f"    {name},")
    w("};")
    w("")
    w("}  // namespace")
    w("")
    w(f"std::size_t GeneratedConfigSource::build_steps() const {{ return {len(steps)}; }}")
    w("")
    w("void GeneratedConfigSource::build_step(::hearthmap::Config &c, std::size_t k) const {")
    w("  if (k < build_steps()) kBuildSteps[k](c);")
    w("}")
    w("")
    w("::hearthmap::Config GeneratedConfigSource::build() const {")
    w("  ::hearthmap::Config c;")
    w("  for (std::size_t k = 0; k < build_steps(); ++k) build_step(c, k);")
    w("  return c;")
    w("}")
    w("")
    w("::hearthmap::ConfigLoadResult GeneratedConfigSource::load() const {")
    w("  using namespace ::hearthmap;")
    w("  // Every source runs validate() and refuses a config with an Error (config.h, D-017).")
    w("  Config c = build();")
    w("  ConfigLoadResult result;")
    w("  result.issues = validate(c);")
    w("  if (usable(result.issues)) result.config = std::move(c);")
    w("  return result;")
    w("}")
    w("")
    if entity_ids:
        w("const SubscribedEntity kSubscribedEntities[] = {")
        for eid in entity_ids:
            w(f"    {{{cpp_string(eid)}}},")
        w("};")
        w(f"const std::size_t kSubscribedEntityCount = {len(entity_ids)};")
    else:
        # A zero-length array is ill-formed in C++; keep one inert entry and a count of zero.
        w("const SubscribedEntity kSubscribedEntities[] = {{nullptr}};")
        w("const std::size_t kSubscribedEntityCount = 0;")
    w("")
    w("}  // namespace hearthmap_gen")
    w("")
    return GeneratedConfig(GENERATED_HEADER, "\n".join(lines), tuple(entity_ids))


# ---------------------------------------------------------------------------------------------
# Photos
# ---------------------------------------------------------------------------------------------


def photo_ids(document: dict) -> list[str]:
    """Distinct photo ids the config's pins name, in first-use order."""
    out: list[str] = []
    for pin in document.get("pins", []):
        pid = pin.get("photo")
        if pid is not None and pid not in out:
            out.append(pid)
    return out


def _image_readable(path: Path) -> bool:
    """True when Pillow can fully decode the file: the same decode the build does later."""
    from PIL import Image, UnidentifiedImageError

    try:
        with Image.open(path) as img:
            img.load()
        return True
    except (OSError, UnidentifiedImageError, ValueError, SyntaxError):
        return False


def check_photos(document: dict, photos_dir: Path | None) -> dict[str, Path]:
    """Map each photo id the config names to `<photos_dir>/<id>.png`. A named photo with no file
    is an error: the editor exports the photos folder with the config, so a missing file is a
    broken export, and a silent placeholder would hide it. Pins with no photo id get ui_lvgl's
    placeholder leaf by design."""
    ids = photo_ids(document)
    if ids and photos_dir is None:
        raise ConfigError("/pins", f"pins name photos ({', '.join(ids)}) but no photos: directory is set")
    found: dict[str, Path] = {}
    for i, pin in enumerate(document.get("pins", [])):
        pid = pin.get("photo")
        if pid is None or pid in found:
            continue
        f = photos_dir / f"{pid}.png"  # type: ignore[operator]
        if not f.is_file():
            raise ConfigError(f"/pins/{i}/photo", f"photo {pid!r} not found: expected {f.name} in the photos directory")
        if not _image_readable(f):
            raise ConfigError(f"/pins/{i}/photo", f"photo {pid!r}: {f.name} is not a readable image")
        found[pid] = f
    return found


def rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-bit channels by truncation: the same rule as core's to_rgb565()."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def to_rgb565_bytes(image: Any, size: int = PHOTO_SIZE) -> bytes:
    """A Pillow image -> size x size RGB565, little-endian (LV_COLOR_FORMAT_RGB565, the native
    uint16 layout on the ESP32-P4). Transparency is flattened onto the plant disc fill; a
    non-square image is centre-cropped to a square before resizing."""
    from PIL import Image  # imported here so the rest of this module needs no Pillow

    img = image.convert("RGBA")
    bg = Image.new("RGBA", img.size, PHOTO_BACKGROUND + (255,))
    img = Image.alpha_composite(bg, img).convert("RGB")
    w, h = img.size
    side = min(w, h)
    left, top = (w - side) // 2, (h - side) // 2
    img = img.crop((left, top, left + side, top + side))
    if img.size != (size, size):
        img = img.resize((size, size), Image.Resampling.LANCZOS)
    out = bytearray()
    raw = img.tobytes()  # packed RGB, row-major
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = rgb565(r, g, b)
        out += bytes((v & 0xFF, v >> 8))
    return bytes(out)


def _c_bytes(data: bytes, indent: str = "    ") -> str:
    rows = []
    for i in range(0, len(data), 24):
        rows.append(indent + ", ".join(f"0x{b:02X}" for b in data[i : i + 24]) + ",")
    return "\n".join(rows)


PHOTOS_HEADER = """\
// Generated by the hearthmap ESPHome component (components/hearthmap). Do not edit.
#pragma once

#include <cstdint>

namespace hearthmap_gen {

/// One converted plant photo at one size: `size` x `size` pixels, RGB565 little-endian
/// (LV_COLOR_FORMAT_RGB565), stride size * 2. Every photo appears once per size ui_lvgl draws
/// discs at, largest first. Plain data, so this header needs no LVGL.
struct PhotoData {
  const char *id;  ///< Photo id from the config.
  const std::uint8_t *pixels;
  std::uint16_t size;
};

extern const PhotoData kPhotos[];
extern const unsigned kPhotoCount;

}  // namespace hearthmap_gen
"""


def emit_photos(photos: dict[str, Path], sizes: tuple[int, ...] = PHOTO_SIZES) -> tuple[str, str]:
    """(header, source) for the converted photos, each at every size in `sizes` (largest first
    in the table). Pins with no photo get ui_lvgl's own placeholder leaf
    (ui_lvgl/include/hearthmap/ui/ports.h), so none is generated here."""
    from PIL import Image

    order = sorted(set(sizes), reverse=True)
    lines = [
        "// Generated by the hearthmap ESPHome component (components/hearthmap). Do not edit.",
        '#include "hearthmap_gen/photos.h"',
        "",
        "namespace hearthmap_gen {",
        "namespace {",
        "",
    ]
    entries = []
    for i, (pid, path) in enumerate(photos.items()):
        with Image.open(path) as img:
            img.load()
            for size in order:
                name = f"kPhoto{i}_{size}"
                entries.append((pid, name, size))
                lines += [f"// {pid}: {path.name} at {size} x {size}",
                          f"alignas(4) const std::uint8_t {name}[] = {{",
                          _c_bytes(to_rgb565_bytes(img, size)), "};", ""]
    lines += ["}  // namespace", ""]
    if entries:
        lines.append("const PhotoData kPhotos[] = {")
        lines += [f"    {{{cpp_string(pid)}, {arr}, {size}}}," for pid, arr, size in entries]
        lines.append("};")
    else:
        lines.append("const PhotoData kPhotos[] = {{nullptr, nullptr, 0}};")
    lines += [
        f"const unsigned kPhotoCount = {len(entries)};",
        "",
        "}  // namespace hearthmap_gen",
        "",
    ]
    return PHOTOS_HEADER, "\n".join(lines)


# ---------------------------------------------------------------------------------------------
# Getting core and ui_lvgl into the firmware build
# ---------------------------------------------------------------------------------------------

# Directories under the firmware build's src/ that this component owns. sync_tree() deletes
# stale files only inside these.
LIB_DIR = "hearthmap_lib"  # library .cpp files (and private headers next to them)
GEN_DIR = "hearthmap_gen"  # generated tables and photos
_SOURCE_SUFFIXES = (".cpp", ".c", ".h", ".hpp")
HOST_SHARED_FILES = ("summary.h", "summary.cpp")


def library_files(repo_root: Path) -> dict[str, Path]:
    """Build-tree path (relative to src/) -> file in this repo, for core and ui_lvgl (plus the
    host's summary stopgap, see below).

    Each library's public `include/` tree is mirrored to the root of src/ (ESPHome's src
    component has "." on its include path, so `#include "hearthmap/config.h"` resolves with no
    extra flags); its `src/` tree goes under hearthmap_lib/<library>/, keeping relative includes
    between private files working. Tests, tools and examples are never copied.
    """
    out: dict[str, Path] = {}
    for lib in ("core", "ui_lvgl"):
        base = repo_root / lib
        for sub, prefix in (("include", ""), ("src", f"{LIB_DIR}/{lib}/")):
            tree = base / sub
            if not tree.is_dir():
                continue
            for f in sorted(tree.rglob("*")):
                if f.is_file() and f.suffix in _SOURCE_SUFFIXES:
                    out[prefix + f.relative_to(tree).as_posix()] = f
    # Stopgap, docs/decisions.md D-030: the top bar's house summary is computed in host/ until
    # core grows a view-model field for it. The device uses the same two files rather than a
    # second implementation; they depend on core only.
    for name in HOST_SHARED_FILES:
        f = repo_root / "host" / "src" / name
        if f.is_file():
            out[f"{LIB_DIR}/host/{name}"] = f
    return out


def public_include_roots(files: dict[str, Path]) -> set[str]:
    """Top-level names under src/ that mirrored include trees occupy (e.g. "hearthmap")."""
    return {k.split("/", 1)[0] for k in files if not k.startswith(LIB_DIR + "/") and "/" in k}


def write_if_changed(path: Path, content: bytes) -> bool:
    if path.is_file() and path.read_bytes() == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return True


def sync_tree(src_root: Path, files: dict[str, bytes], owned_dirs: set[str]) -> list[str]:
    """Write `files` (relative path -> bytes) under src_root, touching only changed files so
    incremental builds stay incremental, and delete files under `owned_dirs` that are no longer
    in the set. Returns the relative paths written."""
    written = [rel for rel, data in sorted(files.items()) if write_if_changed(src_root / rel, data)]
    for d in owned_dirs:
        root = src_root / d
        if not root.is_dir():
            continue
        for f in sorted(root.rglob("*"), reverse=True):
            rel = f.relative_to(src_root).as_posix()
            if f.is_file() and rel not in files:
                f.unlink()
            elif f.is_dir() and not any(f.iterdir()):
                f.rmdir()
    return written
