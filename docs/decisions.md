# Design decisions

Numbered, stable, cited from headers and docs as `D-NNN`. Each entry: the question, the default
chosen, why, and what would change the call. PR 1 set these defaults without an operator
available to ask; each is reversible and marked **provisional** until the operator confirms it
in review. Change one by editing it in place (old answer, new answer, why) in the same PR as the
code, never silently.

### D-001 Coordinates are normalised map space (provisional)

**Question:** what coordinate space do rooms and pins use? **Default:** 0..1 in both axes over
the map area, origin top-left, y down (`geometry.h`). Distances for interpolation and
hit-testing are measured in canvas pixels. **Why:** survives a change of panel, layout or
floor-plan image resolution; the editor can export without knowing the panel. **Would change
if:** non-rectangular map areas or zoom/pan appear (then a transform replaces the rectangle).

### D-002 Core throws no exceptions; errors are values (provisional)

**Default:** no `throw` in core; failures are `std::optional`, `ConfigIssue` lists, and
`Freshness` reasons. No RTTI either. **Why:** ESP-IDF builds commonly disable exceptions and
RTTI; a library that needs them would force flags onto the firmware. **Would change if:** never
for core; host-only code may use exceptions.

### D-003 The air layer is CO2 only (provisional)

**Default:** `air_quality` pins must bind `co2`, and only CO2 feeds the air layer. `pm25` and
`voc` are optional and shown in the overlay. **Why:** one layer needs one unit; mixing ppm with
µg/m³ in one interpolated field is meaningless. CO2 is the common "open a window" signal.
**Would change if:** someone wants a PM2.5 heatmap: add a fifth layer rather than overloading air.

### D-004 Soil and plant pins' temperature/humidity are display-only (provisional)

**Default:** `feeds_layer()` returns false for `temperature`/`humidity` on `soil` and `plant`
pins; every other role on every kind feeds its layer (so a combined temperature + humidity
sensor is one pin feeding two layers). **Why:** a probe in a pot measures the pot (damp,
shaded), which would paint wrong room climate. **Would change if:** a per-binding `feeds`
override is asked for.

### D-005 Heat is room-local; unmeasured rooms show "no data" (provisional)

**Question:** should interpolation cross walls? **Default:** no. A cell takes influence only
from samples in its own room; a room with no sample for the layer is `CellState::NoData`, drawn
as "no data", never a value borrowed from a neighbour. **Why:** principle 1 (claims must hold):
a coloured room asserts a measurement. Walls also really do separate climates (a bathroom after
a shower). **Would change if:** large open-plan rooms want cross-room blending; that would be an
opt-in per room pair, not a global switch.

### D-006 Outdoor rooms are never judged against indoor comfort (provisional)

**Default:** in an `outdoor` room, `temperature`, `humidity` and `co2` assessments are `Ok`
whenever fresh (still coloured on the heat field, never pulsing). Plants outdoors are still
judged by thirst and appear in the HUD. **Why:** 5 °C on a balcony in winter is not a problem to
fix. **Would change if:** outdoor bands are wanted (e.g. frost warnings): add an `outdoor` band
set per layer.

### D-007 Defaults are applied by the config source, not core (provisional)

**Default:** every `ConfigSource` (host JSON parser, generated tables) fills the schema's
defaults; core's structs always hold concrete values. `default_band()` in `config.h` is the one
copy of the band defaults; `schema/README.md` documents the same numbers (an automated check
that they agree is stated, not yet enforced: PR 2). **Why:** one place decides what a default is;
core functions never branch on "was this set". **Would change if:** runtime config editing on the
device arrives (then the device needs the same defaulting, which this already allows).

### D-008 Rooms pulse for the active layer only (provisional)

**Default:** a room pulses at the worst severity among its pins' active-layer assessments; stale
counts as ok for pulsing; a disabled active layer never pulses. **Why:** with many dry plants a
whole-house pulse on every tab would be constant noise; the HUD already shows thirst on every tab.
**Would change if:** the operator wants a cross-layer "something is wrong here" pulse; that would
be a second, quieter pulse level, not a change to this one.

### D-009 HUD: fresh, dry-side plants only; worst first; deterministic ties (provisional)

**Default:** candidates are `plant` pins whose moisture is fresh and above the effective warn
threshold on the dry side. Order: danger before warn, thirst descending, config order. Cap
`hud.max_items`, remainder counted as overflow. Stale plants and too-wet plants are not listed
(their pins show it). `soil` pins never appear. **Why:** the HUD answers one question: what needs
water now. A stale plant may or may not; listing it would make the list untrustworthy. **Would
change if:** a "sensors to check" section is wanted; that is a separate list, not a mix.

### D-010 The soil layer's domain is thirst; happy ranges are in raw units (provisional)

**Default:** the soil layer interpolates and colours thirst (0 wet .. 1 dry), so dry is red
("hot") and over-wet is blue. `happy_min`/`happy_max` are in the sensor's raw units, the same as
`dry`/`wet`, and are converted per pin (`effective_soil_band()`). **Why:** thirst is comparable
across sensors with different raw scales, so one band works house-wide; raw units for the happy
range match what the operator sees in Home Assistant and in the editor. **Would change if:** the
calibrate tool proves better at emitting fractions; the conversion point would move, not the
domain.

### D-011 Core does not convert units (provisional)

**Default:** readings carry HA's unit string, which core ignores. Temperature bands are in
`temperature_unit`; the band defaults are Celsius, and a config source refuses a Fahrenheit
config that leaves any temperature band field to its default. **Why:** unit conversion is a
source of silent wrongness; refusing is louder than converting. **Would change if:** mixed-unit
sensors in one house turn up (then the component converts at ingest, still not core).

### D-012 Staleness boundaries (provisional)

**Default:** stale when `now - last_updated > stale_after` (strictly greater; exactly the timeout
is fresh); a `last_updated` in the future counts as age 0; missing, `unavailable`, `unknown` and
non-numeric states are stale with distinct `Freshness` reasons. `Stale` ranks between `Ok` and
`Warn`. **Why:** clock skew between HA and the panel must not blank the screen; a silent sensor
must be visible but must not outrank a real alarm. **Would change if:** a stale sensor is judged
worse than a warning in practice.

### D-013 UI state is the active layer plus one open overlay (provisional)

**Default:** `UiState` holds only those two. Tapping a pin opens its overlay; tapping the same
pin again closes it; tapping a room or empty map closes it; switching layer keeps it open; a HUD
row opens that plant's overlay. Hit-testing covers the map only (pins first, nearest within
32 px, then rooms); LVGL handles its own widgets (tabs, HUD rows, the close control) and turns
them into `UiEvent`s. **Why:** core stays free of widget layout, and every UI behaviour is still
a pure, tested function. **Would change if:** the overlay grows multiple pages or the map gains
zoom.

### D-014 Deferred work is tracked in GitHub issues on this repo (provisional)

**Default:** GitHub issues, since this repo is public and has no other tracker. Nothing private
goes in an issue. **Would change if:** the operator names a tracker.

### D-015 Calibration edge cases (provisional)

**Default:** an inverted range (wet < dry) is valid and needs no special case; readings beyond
either end clamp; a zero-width range (|wet − dry| < 1e-9) is a config error (`validate()`), and if
core meets one anyway the pin is stale with reason `BadCalibration`, never a guessed thirst.
**Why:** capacitive probes really do read inverted, and dividing by zero must not produce a
confident 0% or 100%.

### D-016 Start-up layer defaults to temperature (provisional)

**Default:** `layers.default` is `temperature`. **Why:** the HUD already lists thirsty plants
on every tab, so the map's first view complements it rather than duplicating it. **Would change
if:** the operator prefers the soil layer first; it is one config field.

### D-017 Heat fields are bit-reproducible; core never fuses multiply-adds (provisional)

**Question:** how deterministic must `compute_heat_field()` be? **Default:** the same inputs give
byte-identical cells, on the same build and across IEEE-754 platforms, for integer `idw_power`
values 1..8. Core is compiled with `-ffp-contract=off` (Clang contracts `a*b+c` into fused
multiply-adds by default, GCC does in GNU modes), never with fast-math; cells and samples are
iterated in a fixed order; integer powers use multiplication and at most one `sqrtf`, both exactly
rounded, instead of `powf`, whose rounding differs between C libraries. A golden hash of the
example flat's field (`core/tests/heat_field_test.cpp`) is checked on every compiler CI runs.
Non-integer powers use `powf` and are reproducible only within one C library. **Why:** golden
images (PR 5) depend on the field; an image that changes with the compiler is a flaky test.
**Would change if:** a non-integer power becomes the default (then a portable power function
belongs in core), or the firmware toolchain needs contraction for speed (it does not today: the
P4 has no double-precision FPU, and the heat field is float).

### D-018 The default palette is the UI spec's dark heatmap (provisional)

**Question:** `Palette`'s defaults were a light warm grey, blue and red that appear nowhere in the
UI spec. **Default:** neutral rgb(30,35,41), cold rgb(42,120,214), hot rgb(227,73,72), the
spec's comfort-quiet diverging colours on its #0d1013 background. **Why:** principle 3 (quiet
when fine): on a dark panel a light grey neutral would make a healthy house the brightest thing on
the wall. **Would change if:** a light theme is added; it passes its own `Palette`.
