# schema/

`hearthmap.schema.json` is the one config contract (JSON Schema 2020-12). The editor writes it,
the ESPHome component compiles it into static tables, the host renderer parses it, and core's
`Config` struct (`core/include/hearthmap/config.h`) mirrors it field for field.
`readings.schema.json` describes the host's readings fixtures (`examples/README.md`).
`calibration.schema.json` describes `calibration.json`, the proposals `tools/calibrate/` writes
and the editor imports; its `calibration` objects are this schema's `$defs/calibration`.

The schema checks shape. Rules about values that a schema cannot express (unique ids, a pin's
room existing, band ordering, calibration width, polygon simplicity) belong to core's
`validate()`, declared in `config.h`. `validate()` also re-checks every range and pattern this
schema states, because the device's generated tables never pass through a JSON Schema
validator: no config source may rely on the schema having run (`docs/decisions.md` D-017).
`validate.py` checks only ids and cross-references; core's `validate()` itself runs on the
example through the host's tests and `hearthmap-render --check` (D-048), and
`semantic-cases.json` holds the schema-valid configs `validate()` must refuse or accept, read by
core's and the editor's tests alike.

```sh
python3 -m venv .venv && .venv/bin/pip install -r schema/requirements.txt
.venv/bin/python schema/validate.py      # every examples/*/config.json and fixture
.venv/bin/python schema/test_schema.py   # negative controls: mutations that must be rejected
```

## Coordinates

Two spaces (`core/include/hearthmap/geometry.h`):

- **Map space**: rooms and pins. `[x, y]`, each 0..1, origin at the **top-left** of the map area,
  x to the right, y **down**. Normalised so a config survives a change of panel or layout. The
  map area's aspect ratio decides how map space is stretched; the editor exports in the floor
  plan's own proportions, so set `canvas.map_area` to the same aspect ratio as the floor plan
  or the plan will look squashed.
- **Canvas space**: panel pixels after rotation, origin top-left. Only `canvas` uses it.

## Top level

| Field | Type | Default | Meaning |
|---|---|---|---|
| `$schema` | string | | Optional pointer to this file, for editors. |
| `version` | `1` | required | Schema version. Anything else is refused. |
| `canvas` | object | required | Panel size and where the map sits (below). |
| `temperature_unit` | `"C"` \| `"F"` | `"C"` | Unit of temperature readings **and** of temperature band values. Core does not convert: if the readings are Fahrenheit, the bands must be too. |
| `rooms` | array, 1..64 | required | Room polygons (below). Order matters: where polygons overlap, the earlier room owns the overlap. |
| `pins` | array, 0..256 | required | Sensor pins (below). Order is the tie-breaker wherever a tie can occur (hit-testing, HUD order). |
| `layers` | object | all defaults | Per-layer bands and thresholds (below). |
| `stale_after_s` | integer >= 1 | `10800` (3 h) | A reading whose `last_updated` is more than this many seconds before now is stale. Exactly this old is still fresh. On the panel, `last_updated` means "last confirmed": every value is re-stamped while Home Assistant is connected (D-012), so this timeout applies only while it is disconnected, and a probe that dies without going `unavailable` looks fresh (issue 11; D-046). The host takes fixture timestamps as given. |
| `hud` | object | | `max_items` (1..20, default `6`): thirsty plants listed before "+N more". |
| `heat` | object | | Heat-field tuning: `grid_cols` (2..256, default `48`), `grid_rows` (2..256, default `40`), `idw_power` (>0..8, default `2`). |

## `canvas`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `width`, `height` | integer | required | Pixels after rotation. The target panel is 1280 x 800. |
| `orientation` | `"landscape"` \| `"portrait"` | required | Landscape needs `width > height`, portrait `height > width` (core `validate()`). |
| `map_area` | `{x, y, width, height}` | the whole canvas: `{0, 0, width, height}` | Canvas rectangle that map space is drawn into; must lie inside the canvas. The rest of the canvas is the HUD and layer tabs, so a real config sets it (the example uses `{0, 0, 960, 800}`). |

## `rooms[]`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `id` | `^[a-z][a-z0-9_]{0,47}$` | required | Unique among rooms. Pins refer to it. |
| `name` | string, 1..48 | required | Display name. |
| `polygon` | 3..64 map points | required | Implicitly closed: do not repeat the first vertex. Must be simple (not self-intersecting). Winding order does not matter. |
| `outdoor` | boolean | `false` | Outdoor rooms (a balcony) are never judged against indoor comfort bands: their temperature, humidity and CO2 pins are always "ok" when fresh, and show their values. They get no heat colour: drawn in the outdoor treatment instead (`docs/decisions.md` D-006). Plants outdoors are still judged by thirst. |

## `pins[]`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `id` | `^[a-z][a-z0-9_]{0,47}$` | required | Unique among pins. |
| `kind` | enum | required | `temperature`, `humidity`, `soil`, `air_quality`, `plant`. Decides the icon, which role is required, and whether the pin can appear in the HUD (plants only). |
| `name` | string, 1..48 | required | Display name; for plants, the plant's name. |
| `position` | map point | required | Pin centre. Should lie inside its room's polygon (core warns if not). |
| `room` | room id | required | The room the pin is in. |
| `entities` | object | required | Home Assistant entity ids by role (below). |
| `photo` | `^[a-z0-9][a-z0-9_-]{0,47}$` | none | Plant pins only (core `validate()` rejects it elsewhere too). Resolved by the editor and the ESPHome component to `photos/<id>.png`; core never touches the file. |
| `calibration` | object | required for `soil` and `plant`, forbidden otherwise | Below. |
| `stale_after_s` | integer >= 1 | top-level value | Per-pin staleness override (soil sensors often report rarely). |

### `entities`: roles

| Role | Unit | Feeds layer | Required for kind |
|---|---|---|---|
| `temperature` | `temperature_unit` | Temperature | `temperature` |
| `humidity` | %RH | Humidity | `humidity` |
| `moisture` | raw sensor units | Soil (as thirst) | `soil`, `plant` |
| `co2` | ppm | Air | `air_quality` |
| `pm25` | µg/m³ | none (shown in the overlay) | |
| `voc` | sensor index | none (shown in the overlay) | |
| `battery` | % | none (shown in the overlay) | |
| `predicted_dry_at` | | none: **reserved** for a later watering-time model; accepted and ignored in version 1 | |

Any kind may bind any optional role **except** `moisture`, which only `soil` and `plant` pins may
bind: it needs a calibration, and only those kinds carry one (`docs/decisions.md` D-018). A role
feeds its layer for every kind **except** that `temperature` and `humidity` on `soil` and `plant`
pins are display-only: a probe in a pot measures the pot, not the room (D-004).

The detail overlay lists a pin's roles in the order of this table, whatever order the JSON
object has (D-019).

### `calibration`

Soil sensors disagree about what "dry" means, and a raw `0` is often a real reading of a
bone-dry pot. Thirst is therefore judged against each sensor's own range, typically derived from
its history (`tools/calibrate/`).

| Field | Type | Meaning |
|---|---|---|
| `dry` | number | Raw reading when the soil is as dry as it gets. |
| `wet` | number | Raw reading just after a thorough watering. May be numerically **below** `dry` (capacitive probes reporting an ADC count read lower when wet): an inverted range is valid. Must differ from `dry`. |
| `happy_min` | number, optional | Driest raw reading the plant is happy at. Between `dry` and `wet`, inclusive. |
| `happy_max` | number, optional | Wettest raw reading the plant is happy at. Between `dry` and `wet`, inclusive, and not drier than `happy_min`. |

Core computes **thirst** = 1 − (raw − dry) / (wet − dry), clamped to 0..1: 0 is just watered, 1 is
bone dry. `happy_min` and `happy_max` replace the soil layer's warn edges for that one plant
(exact algorithm: `core/include/hearthmap/calibration.h`, `effective_soil_band()`).

## `layers`

`default` (`temperature` | `humidity` | `soil` | `air`, default `temperature`) is the layer shown
at start-up and must be enabled. Each of `temperature`, `humidity`, `soil`, `air` is an optional
object with these optional fields; an omitted field takes the default below, and a threshold set
to `null` removes that threshold.

| Field | Meaning |
|---|---|
| `enabled` | Default `true`. A disabled layer has no tab, no heat field and no pulsing, and the readings that feed it are not judged (no badge); their values still show in the overlay. The plant HUD works regardless (`docs/decisions.md` D-020). |
| `comfort_min`, `comfort_max` | The comfort band: drawn in the neutral colour. Affects colour only, never severity. |
| `warn_below`, `warn_above` | Strictly beyond these: **warn**. |
| `danger_below`, `danger_above` | Strictly beyond these: **danger**. A value exactly on a threshold gets the less severe side. |
| `scale_min`, `scale_max` | Where the cold (blue) and hot (red) colours saturate. If `scale_min` is not below `comfort_min`, the cold side stays neutral (no colour), and likewise the hot side (D-021). |

Ordering, checked by core: `scale_min <= danger_below <= warn_below <= comfort_min <= comfort_max
<= warn_above <= danger_above <= scale_max`, for every threshold present.

Defaults (`default_band()` in `config.h` holds the same numbers):

| Layer | Domain | comfort | warn below / above | danger below / above | scale |
|---|---|---|---|---|---|
| `temperature` | °C | 19–24 | 16 / 27 | 12 / 32 | 10–34 |
| `humidity` | %RH | 40–60 | 30 / 65 | 20 / 75 | 15–90 |
| `soil` | thirst 0..1 | 0.1–0.6 | none / 0.7 | none / 0.9 | 0–1 |
| `air` | CO2 ppm | 400–800 | none / 1200 | none / 2000 | 350–2500 |

The temperature defaults are Celsius. With `temperature_unit: "F"`, `layers.temperature` must be
present with all eight band fields above set explicitly (`null` is fine for a threshold; `enabled`
is not a band field): core does not convert. The schema states this with an `if`/`then`, and core's
`validate()` enforces it; config sources only record which fields were present
(`docs/decisions.md` D-011).

## Changing the schema

A schema change is a contract change: bump `version` for anything that is not purely additive
with a default, update `config.h`, this README, the example, and `test_schema.py` in the same PR.
