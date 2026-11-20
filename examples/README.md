# examples/

Public-safe examples only. Everything here is **fictional**: the flat, its rooms, its plants and
every entity id (all `sensor.example_*`). A real house's config lives in a private repository
and never here (`AGENTS.md` §4).

## `flat/`: a six-room flat with a balcony

`flat/config.json` is a fictional flat on a 12 x 10 grid (map area 960 x 800 px, so one grid unit
is 80 px in both directions and map distances are undistorted):

```
x: 0          5     8          12
   +----------+-----+----------+  y=0
   |          |Kitch| Study    |
   |  Living  +-----+----------+  y=4
   |          | Hallway        |
   +----------+   +-----+------+  y=6
   | Balcony  |Hal|Bath-| Bed- |
   | (outdoor)|l  |room | room |
   +----------+---+-----+------+  y=10
              5   7    9.5    12
```

Rooms: living (0–5 x 0–6), kitchen (5–8 x 0–4), study (8–12 x 0–4), an L-shaped hallway, bathroom
(7–9.5 x 6–10), bedroom (9.5–12 x 6–10), and an outdoor balcony (0–5 x 6–10).

Pins: six climate pins (one outdoor), two air-quality pins, one soil probe in a balcony planter
(a `soil` pin: on the soil layer, never in the HUD), and eight plants chosen to exercise
calibration: an ordinary 0–100-ish percentage (monstera), a happy range on both sides (fiddle-leaf
fig), a sensor that really reads 0% (basil, which also binds the reserved `predicted_dry_at`), an
**inverted** ADC range, dry 2900 / wet 1300 (pothos), a one-sided happy range (fern), and a per-pin
stale override of 6 h (cherry tomato). The HUD cap is 4 so a fixture can overflow it.

The air layer's `warn_above` is lowered to 1000 ppm to show a partial layer override.

## Readings fixtures

`flat/readings/*.json` are snapshots of Home Assistant state for the host renderer and tests.
Format (`schema/readings.schema.json`):

```json
{
  "description": "free text: what this fixture shows",
  "now": "2026-01-15T09:00:00Z",
  "readings": {
    "sensor.example_living_temperature": {
      "value": 21.5,
      "unit": "°C",
      "last_updated": "2026-01-15T08:55:00Z"
    }
  }
}
```

- `now`: the moment the snapshot represents, passed to core as `now`. RFC 3339 with an explicit
  offset.
- `readings`: entity id → reading. An entity bound in the config but **absent** here is
  *missing* (never received), which core reports as stale.
- `value`: a number, or the literal HA states `"unavailable"` / `"unknown"`.
- `unit`: HA's `unit_of_measurement`, informational (core does not convert). Optional.
- `last_updated`: when HA last changed or refreshed the state. RFC 3339 with an offset.

Every entity a fixture mentions must be bound by a pin in the config (`schema/validate.py`
checks), so a typo cannot silently test nothing.

### What each fixture should show

Thirst values below are 1 − (raw − dry) / (wet − dry). The view-model tests (PR 4) and golden
images (PR 5) assert these outcomes; if a rule in `docs/decisions.md` changes, update this table
in the same PR.

**`all-ok`**: every sensor fresh, inside its comfort band. The balcony is 9 °C and 80% RH but
outdoor, so it is "ok". Nothing pulses on any layer. HUD empty.

**`thirsty-many`** (soil layer; HUD cap 4):

| Pin | Raw | Thirst | Severity |
|---|---|---|---|
| plant_basil | 0 | 1.000 | danger (a real 0%) |
| plant_monstera | 6 | 0.982 | danger |
| plant_lavender | 2 | 0.960 | danger |
| plant_pothos | 2750 (inverted range) | 0.906 | danger |
| plant_snake | 9 | 0.838 | warn |
| plant_fern | 38 | 0.646 | warn (its happy_min 40 moves warn_above to 0.615) |
| plant_tomato | 40 | 0.397 | ok |
| plant_fiddle | 45 | 0.417 | ok |
| soil_planter | 4 | 0.949 | danger, but a soil pin: not in the HUD |

HUD: basil, monstera, lavender, pothos, then "+2 more" (snake, fern). Pulsing on the soil layer:
living, kitchen, study, balcony at danger; bedroom and bathroom at warn.

**`stale-sensors`**:

| Entity | Situation | Expected |
|---|---|---|
| kitchen temperature | last updated 12 h before now | stale (too old) |
| study temperature | 3.5 h old, default timeout 3 h | stale (too old) |
| bathroom humidity | `"unavailable"` | stale (unavailable) |
| snake plant moisture | `"unknown"` | stale (unknown) |
| bedroom CO2 | absent from the fixture | stale (missing) |
| cherry tomato moisture | 4 h old, pin override 6 h | fresh |
| lavender moisture | exactly 3 h old | fresh (the boundary is not stale) |

Stale pins show as stale and never pulse a room.

**`humid-bathroom`** (humidity layer): bathroom 82% RH (danger, above 75) pulses at danger;
bedroom 66% (warn, above 65) pulses at warn; balcony 92% is outdoor and does not pulse. The
bathroom fern is well watered (thirst 0.154).
