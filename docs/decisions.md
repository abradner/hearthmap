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
for core; host-only code may use exceptions. **Amended (contract round 1, R15):** was stated only;
now enforced: `hearthmap_core` compiles with `-fno-exceptions -fno-rtti` (MSVC `/GR- /EHs-c-`),
including the header self-containment units, so a header that throws or uses RTTI fails the build.
The test executable keeps exceptions because doctest uses them.

### D-003 The air layer is CO2 only (provisional)

**Default:** `air_quality` pins must bind `co2`, and only CO2 feeds the air layer. `pm25` and
`voc` are optional and shown in the overlay. **Why:** one layer needs one unit; mixing ppm with
µg/m³ in one interpolated field is meaningless. CO2 is the common "open a window" signal.
**Would change if:** someone wants a PM2.5 heatmap: add a fifth layer rather than overloading air.
**Note (follow-up review):** this is about the heat layer only. The top bar's house summary also
shows a mean of fresh indoor PM2.5 (D-030, D-049), as a number, not judged and not a layer.

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
opt-in per room pair, not a global switch. **Amended (contract round 1):** (R4) the coincidence
rule is room-local too: a cell takes a sample's value exactly only when that sample is in the
cell's own room; before, "any sample within 0.5 px" could let a sample across a wall set a cell.
(R10) outdoor rooms are no longer "rooms like any other" here: their cells are `Outside` (D-006).
(R11) the interpolation runs in `float`, not `double` (D-022).

### D-006 Outdoor rooms are never judged against indoor comfort (provisional)

**Default:** in an `outdoor` room, `temperature`, `humidity` and `co2` assessments are `Ok`
whenever fresh (never pulsing). Plants outdoors are still judged by thirst and appear in the HUD.
**Amended (contract round 1, R10):** outdoor rooms were coloured on the heat field; now they get no
heat colour. `compute_heat_field()` marks their cells `Outside` and ignores samples in them; the
UI draws them in the outdoor treatment from the UI spec, using `RoomMask` to find them. Outdoor
pins still show their values. Why: a coloured room asserts a judgement against a comfort band,
and outdoor rooms have none, so colouring them contradicted this entry's own rule. **Why:** 5 °C on a balcony in winter is not a problem to
fix. **Would change if:** outdoor bands are wanted (e.g. frost warnings): add an `outdoor` band
set per layer.

### D-007 Defaults are applied by the config source, not core (provisional)

**Default:** every `ConfigSource` (host JSON parser, generated tables) fills the schema's
defaults; core's structs always hold concrete values. `default_band()` in `config.h` is the one
copy of the band defaults; `schema/README.md` documents the same numbers. ctest's
`core.defaults_match_docs` (`core/tests/check_defaults.py`) compares the compiled defaults with
that table, the README's other default cells and every `default` keyword in the schema, and fails
on any difference; `core.defaults_check_self_test` plants a mismatch in each source and fails
unless every one is caught (was: "stated, not yet enforced: PR 2"). Sources also record which band fields the
config set (`LayerConfig::present`), so `validate()` can judge the Fahrenheit rule (D-011) without
the source re-implementing it. **Why:** one place decides what a default is;
core functions never branch on "was this set". **Would change if:** runtime config editing on the
device arrives (then the device needs the same defaulting, which this already allows).

### D-008 Rooms pulse for the active layer only (provisional)

**Default:** a room pulses at the worst severity among its pins' active-layer assessments; stale
counts as ok for pulsing; a disabled active layer never pulses. **Amended (contract round 1,
R6):** `ViewModel::active_layer` is the layer actually shown (after substitution by
`default_layer`), not the requested one; roles feeding a disabled layer are not judged (D-020). **Why:** with many dry plants a
whole-house pulse on every tab would be constant noise; the HUD already shows thirst on every tab.
**Would change if:** the operator wants a cross-layer "something is wrong here" pulse; that would
be a second, quieter pulse level, not a change to this one.

### D-009 HUD: fresh, dry-side plants only; worst first; deterministic ties (provisional)

**Default:** candidates are `plant` pins whose moisture is fresh and on the dry side.
**Amended (contract round 1, R2):** "dry side" was "above the effective `warn_above`", which is
undefined when `warn_above` is null. Now: thirst > the effective band's `comfort_max` AND
`classify()` gives Warn or Danger. Because roles feeding a disabled layer are not judged (D-020),
the HUD calls `classify()` on the assessment's band itself instead of reading its severity, so it
keeps working with the soil layer disabled (this resolution is the contract-fix builder's call,
taken to keep this entry's "independent of the soil layer" promise; reversible). Order: danger before warn, thirst descending, config order. Cap
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
`temperature_unit`; the band defaults are Celsius, and a Fahrenheit config that leaves any
temperature band field to its default is refused. **Amended (contract round 1, R7):** the refusal
was assigned to every config source; now it is enforced once, in `validate()`, from
`LayerConfig::present`, and mirrored in the schema with `if`/`then`. `layers.temperature` must be
present with all eight band fields; `null` counts as set where the schema allows null; `enabled` is
not a band field. Why: a rule implemented per source is a rule that drifts per source. **Why:** unit conversion is a
source of silent wrongness; refusing is louder than converting. **Would change if:** mixed-unit
sensors in one house turn up (then the component converts at ingest, still not core).

### D-012 Staleness boundaries (provisional)

**Default:** stale when `now - last_updated > stale_after` (strictly greater; exactly the timeout
is fresh); a `last_updated` in the future counts as age 0; missing, `unavailable`, `unknown` and
non-numeric states are stale with distinct `Freshness` reasons. `Stale` ranks between `Ok` and
`Warn`. **Why:** clock skew between HA and the panel must not blank the screen; a silent sensor
must be visible but must not outrank a real alarm. **Would change if:** a stale sensor is judged
worse than a warning in practice. **Amended (contract round 1):** (R8) `last_updated` means "last
confirmed", not "last changed": Home Assistant pushes only on change, so the device glue
re-stamps every valid reading with the current time while the API is connected and stops when it
disconnects; fixtures and the host pass timestamps as given (`readings.h`). The 3-hour default
stays. (R13) when several reasons apply, precedence is Missing > Unavailable > Unknown >
BadCalibration > TooOld (`severity.h`).

### D-013 UI state is the active layer plus one open overlay (provisional)

**Default:** `UiState` holds only those two. Tapping a pin opens its overlay; tapping the same
pin again closes it; tapping a room or empty map closes it; switching layer keeps it open; a HUD
row opens that plant's overlay. Hit-testing covers the map only (pins first, nearest within
32 px, then rooms); LVGL handles its own widgets (tabs, HUD rows, the close control) and turns
them into `UiEvent`s. **Amended (contract round 1, R12):** while an overlay is open it is modal:
a tap on the scrim sends only `CloseOverlay`, a tap on the card sends nothing, and `TapMap` is sent
only when no overlay is open (`view_model.h`, `UiEvent`). **Why:** core stays free of widget layout, and every UI behaviour is still
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
confident 0% or 100%. **Clarified (contract round 1):** the dry..wet span is closed; a happy edge
equal to `dry` or `wet` is inside it.

### D-016 Start-up layer defaults to temperature (provisional)

**Default:** `layers.default` is `temperature`. **Why:** the HUD already lists thirsty plants
on every tab, so the map's first view complements it rather than duplicating it. **Would change
if:** the operator prefers the soil layer first; it is one config field.

### D-017 `validate()` enforces every schema range (contract round 1, R3)

**Default:** core's `validate()` re-checks every range, pattern and cap the JSON Schema states
(positions and vertices in 0..1, grid, canvas, `max_items`, `idw_power`, vertex count, id pattern
and length, photo on plants only, at least one room), as well as the cross-checks. The schema gains
`maxItems` 64 for rooms and 256 for pins. Every `ConfigSource` runs `validate()`. **Why:** the
device's config arrives as generated tables that never pass a JSON Schema validator, so a range
stated only in the schema is a claim nothing on the device enforces. **Would change if:** never
while any source can skip the schema.

### D-018 Moisture binds only on soil and plant pins (contract round 1, R1)

**Default:** the schema rejects `moisture` on other kinds, `validate()` reports it as an error, and
`feeds_layer(kind, Moisture)` is false for other kinds. **Why:** moisture becomes thirst only
through a calibration, and only soil and plant pins carry one. **Would change if:** a combined
sensor kind with its own calibration appears.

### D-019 Overlay rows follow the `Role` enum order (contract round 1, R5)

**Default:** the detail overlay lists a pin's roles in `Role` order, never binding or JSON key
order. **Why:** JSON object order is not guaranteed to survive a parser or the code generator, and
the same pin must look the same on host and device.

### D-020 Disabled layers are not judged (contract round 1, R6)

**Default:** roles that feed a disabled layer get severity `Ok` (no badge, no pulse); their
freshness, values and band are still filled, and the overlay still shows them. The HUD judges
thirst itself (D-009), so it is unaffected. **Why:** turning a layer off says "I do not want to be
told about this quantity"; a badge for it would contradict that.

### D-021 Air scale starts below comfort; an empty side is neutral (contract round 1, R9)

**Default:** the air layer's default `scale_min` is 350 ppm (`comfort_min` stays 400). When
`comfort_min <= scale_min` the cold side is neutral rather than a step to full cold, and likewise
the hot side when `scale_max <= comfort_max`. **Why:** outdoor air is about 420 ppm, so with the old
400/400 default any reading just under 400 jumped straight to full blue: a loud colour for nothing.

### D-022 Performance contract for the device (contract round 1, R11)

**Default:** the ESP32-P4 has a single-precision FPU only, so interpolation and `HeatSample` use
`float` (`MapPointF` for sample positions). Colour is resolved per heat cell, never per screen
pixel, through a `ColourLut` (band + palette -> N `Rgb565`) built once per band. Reuse forms write
into caller-owned storage (`build_room_mask`, `compute_heat_field`, `heat_samples`,
`build_view_model`, `build_colour_lut`); the returning forms wrap them. Integer powers 1..8 use
multiplication and at most one `sqrtf` (D-024; the ruling's "`idw_power == 2` multiply-only" is
the special case); only non-integer powers call `powf`. Per-frame paths do not allocate once
warmed up. **Why:** double arithmetic is software-emulated on that chip, and a field per frame
(the schema default and the example are 48 x 40; the panel's config is 131 x 105) is cheap only if
it stays in float and off the heap. **Corrected (follow-up review):** this entry used to say
"other powers may use `powf`" and quoted only the 48 x 40 grid; D-024 already took integer powers
off `powf`, and real configs use finer grids. **Would change if:** profiling on the panel says
otherwise.

### D-023 `map_area` defaults to the whole canvas (contract round 1, R14)

**Default:** an omitted `map_area` is `{0, 0, canvas.width, canvas.height}`; it was the left 960 x
800 of a 1280 x 800 canvas, a default that only made sense for one panel. The example sets it
explicitly. **Why:** a default should not encode one layout's HUD width.

### D-024 Heat fields are bit-reproducible; core never fuses multiply-adds (provisional)

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

### D-025 The default palette is the UI spec's dark heatmap (provisional)

**Question:** `Palette`'s defaults were a light warm grey, blue and red that appear nowhere in the
UI spec. **Default:** neutral rgb(30,35,41), cold rgb(42,120,214), hot rgb(227,73,72), the
spec's comfort-quiet diverging colours on its #0d1013 background. **Why:** principle 3 (quiet
when fine): on a dark panel a light grey neutral would make a healthy house the brightest thing on
the wall. **Would change if:** a light theme is added; it passes its own `Palette`.

### D-026 Choices the core implementations made where the contract was silent (provisional)

Each is the reversible reading; each has a test that would change with it.

- **A non-finite number in a `Value` reading is `Unknown`** (`assess_freshness`), though sources
  promise never to send one: showing it would print `nan`.
- **A calibration span is usable** only when dry, wet and wet - dry are finite and |wet - dry|
  is at least `kMinCalibrationWidth`: one predicate, `calibration_span_usable()`, used by both
  `validate()` and `moisture_fraction()`, so a valid config never reports `BadCalibration`.
- **Moisture on a pin with no calibration** (only possible in an invalid config) is treated like
  a degenerate calibration: `BadCalibration`, in the D-012 precedence.
- **Polygon simplicity**: a zero-length edge (a repeated vertex, including the first vertex
  repeated at the end), zero area (every vertex on one line, by the same test
  `point_in_polygon()` uses to enclose nothing), or an edge that folds back over its neighbour
  makes a polygon non-simple. Both are checked explicitly, not left to the crossing tests.
  When a vertex is out of range, only the per-vertex error is reported (`/rooms/i/polygon/j`),
  not also "not simple"; when a pin's position is out of range, the outside-its-room warning is
  skipped. `validate()` reports every problem in one pass.
- **HUD ordering** uses a stable insertion sort (no allocation, n <= 256). Under the current
  `effective_soil_band()`, "Danger before Warn" never contradicts "thirst descending" (a Warn
  plant's thirst is at most its `danger_above`, which is at least the layer's), so that rule has
  no observable test today; it is implemented as written in case the band algorithm changes.
- **Overlay storage reuse**: a reused `ViewModel` keeps its overlay rows' storage while an
  overlay stays open; closing it releases them, and reopening allocates once. Each row's unit
  is reserved to `kOverlayUnitCapacity` (16 bytes) when the row is created, so a reading that
  arrives later with a real unit does not allocate; a longer unit is shown in full and may
  allocate (`Reading::unit` has no length bound, so no fixed buffer can promise more).
- **HUD storage** is reserved for every Plant pin (all candidates are collected before the cut
  to `max_items`), so warming up on healthy readings does not leave a later thirsty frame to
  allocate.

### D-027 Calibration proposals: method defaults (provisional, PR 8)

**Default:** `tools/calibrate` takes `wet` as the wetter of the median post-watering plateau and
the 95th time-weighted wetness percentile, and `dry` as the drier of the median pre-watering
trough (with at least two waterings) and the 5th percentile: top-ups of a still-moist pot have
troughs far wetter than its dry spells. A watering is a rise of at least 20% of the sensor's
span within 3 h. Samples hold until the next one (Home Assistant records only changes), for at
most 7 days. A run of one value longer than 72 h is a frozen sensor, ignored, when it is
mid-range; at either end of the range it is kept and reported as saturation, because a pot
that reads 0 for days is the case this project exists for. Polarity is detected from the
direction of sharp moves, else from the slow trend, and can be forced. **Why:** the brief's
method, with defaults chosen against synthetic series; the README states each one. **Would
change if:** real histories show a different watering signature (slow bottom watering, for
one), which would need a different event detector rather than retuned numbers.

### D-028 Default happy range is 30% to 100% of dry to wet (provisional, PR 8)

**Default:** the proposed `happy_min` is 30% of the way from `dry` to `wet` and `happy_max` is
`wet` itself, both labelled defaults (`happy_is_default: true`) to be tuned per species.
**Why:** a `happy_max` short of `wet` would make every just-watered plant warn "too wet"
(`effective_soil_band()` turns `happy_max` into the warn edge on the wet side), and history
holds no evidence of over-watering. 30% gives thirst 0.7, the soil layer's default warn edge, so
adding the default changes no plant's warning point. **Would change if:** per-species presets
arrive in the editor.

### D-029 calibration.json lists sensors as an array (provisional, PR 8)

**Default:** `schema/calibration.schema.json`: `sensors` is an array of objects keyed by
`entity_id`, each carrying `status`; only `status: "ok"` carries a `calibration`, whose shape is
the config's `$defs/calibration` by `$ref`, with all four fields present. Sensors that cannot be
calibrated are listed with a `message` rather than left out, so the editor can say why.
**Why:** an array keeps the tool's order and leaves room for more than one proposal per entity
later; reusing the config's calibration definition means the two cannot drift. **Would change
if:** the editor's import needs something else; it is the only consumer.

### D-030 The top bar's house summary is computed on the host for now (provisional)

**Question:** the UI spec's top bar shows "House 24.2°   68% RH   PM2.5 2", but the view model
carries no house-level figure. **Default:** the embedder formats the summary text and passes it to
`Screen::update()`; the host computes it (`host/src/summary.cpp`) as the mean of fresh
temperature and humidity assessments that feed their layers on indoor pins, and the mean fresh
indoor PM2.5, using core's `assess()` and `feeds_layer()`. A part with no fresh reading is left
out. **Why:** keeps ui_lvgl free of logic and unblocks the screen tonight. This is a principle-2
stopgap: averaging is logic a test could check, so it belongs in core. **Would change if:** core
grows a `ViewModel` summary field (then the host function goes, and the device gets the same
numbers). **Superseded by D-049.**

### D-031 Choices the UI made where the spec was silent (provisional)

Each is reversible and visible in the golden images.

- **Room pulse colour follows severity** (warn amber, danger red); the spec names only the warn
  colour, for its humidity example.
- **Rooms with no data** for the active layer are drawn as dark diagonal stripes, so "no sensor"
  never reads as a colour (D-005). Outdoor rooms get the spec's garden fill without rounded
  corners.
- **Pin badges stay at full strength on dimmed (off-layer) pins**, so a thirsty plant is badged
  on every tab like its HUD row; the danger pulse runs only on the active layer.
- **Plain pin values sit on a dark translucent pill**, so a value in its status colour reads on
  heat of the same colour.
- **Soil pins** (a probe with no plant) use the plant disc and moisture ring with no leaf.
- **The HUD's empty state** reads "Nothing needs water" and hides the count.
- **The overlay's sparkline** section says "No history yet": no source of history exists in the
  contract. The plant overlay always shows its three stats (soil temp, air humidity, battery),
  with "none" where the pin binds no such sensor.
- **Thirst verdicts**: danger dry "water now", warn dry "water soon", warn or danger wet (thirst
  below comfort) "too wet", ok "happy", stale "no data".
- **Fonts**: built-in Montserrat at even sizes, so the spec's 21 px is 20 px.
- **Room labels move rather than hide under pins**: limited to the room's width with an
  ellipsis, dropped below three characters, and placed in the first free corner of the room
  (ui_lvgl/README.md, "Layout rules"). The HUD gap shrinks (not the cards) so seven cards and
  "+N more" fit.

**Would change if:** the operator marks any of these against mockup A.

### D-032 The example flat's map area matches the UI spec's map region (provisional)

**Default:** `examples/flat/config.json` sets `map_area` to x 40, y 76, 864 x 720: inside the
spec's map region (x 0..920, y 64..800, about 40 px left margin), clear of the 64 px top bar and
the HUD column, and at the flat's 12:10 aspect (72 px per grid unit). **Why:** core hit-tests and
interpolates in `map_area`, so the screen must draw the map exactly there, and the previous
0,0,960,800 overlapped both the top bar and the HUD. **Would change if:** the layout changes; the
map area is one config field. The editor's default for new projects now follows the UI's free
region too (D-044, amended).

### D-033 (unused)

Intentionally unused: numbers were handed out to parallel builders on one night, and this one was
never claimed. Kept empty so later references do not shift.

### D-034 core and ui_lvgl reach the firmware by a build-time copy (provisional)

**Question:** ESPHome copies only a component directory's top-level files into the build, and
`core/` and `ui_lvgl/` live outside `components/hearthmap/`. How do they get compiled? **Default:**
the component's Python copies each library's `include/` tree to the root of the build's `src/` and
its `src/` tree to `src/hearthmap_lib/<lib>/` (plus `host/src/summary.*`, the D-030 stopgap),
writing only changed files and deleting stale ones in those directories. ESPHome 2026.11.0 clones
the whole repository for a `github://` source and a local source is a working tree, so the files
are always beside the component; the generated `src/CMakeLists.txt` globs `src/**` and has `src/`
on the include path. **Why:** one ref pins component and library together, no network beyond
ESPHome's own clone, a local path builds the working tree, and no second build description of core
is needed. Rejected: `add_idf_component` (a second fetch at a ref the component cannot know, and an
IDF-specific CMakeLists in core), `cg.add_library` with a git URL (same), a vendored copy with a
CI equality check (two copies kept equal by a check, not by construction). Details:
`components/hearthmap/README.md`. **Would change if:** ESPHome stops cloning the whole repository,
or core gains an IDF component description for other reasons.

### D-035 Device config errors: schema at build time, validate() at boot, never a guess (provisional)

**Default:** the component checks the config against the JSON Schema while ESPHome validates the
YAML (so `esphome config` fails), with a small built-in checker because ESPHome's image has no
`jsonschema`; it refuses any schema keyword it does not implement, and its tests compare it with
`jsonschema`. Semantic rules stay in core: the generated `ConfigSource` runs `validate()` at boot,
and a refused config draws its errors on screen, logs every issue with its path and sets the
component's error status; nothing else is rendered. A photo id with no `<id>.png` fails the build;
a plant with no photo id gets ui_lvgl's placeholder. **Why:** D-017 puts enforcement in one place;
a second Python copy of `validate()` would drift. Running `validate()` at boot rather than at build
time is the honest form available without a host compiler in the build. **Would change if:** the
build gains a host compile step, in which case the same generated tables could be validated before
flashing.

### D-036 Device readings: what is subscribed, and before time is valid (provisional)

**Default:** one Home Assistant state subscription per distinct bound entity, plus one for its
`unit_of_measurement` attribute (`Reading::unit`); `predicted_dry_at` bindings are not subscribed
(core ignores the role in v1). R8 re-stamping applies to readings in state Value only, on every
rebuild while the API connection with a state subscription is up. Until the configured time source
is valid the screen says "Waiting for time" and nothing is judged; readings received before that
carry time 0 until the first re-stamp. **Why:** a unit is part of what the overlay shows; HA
confirming "unavailable" says nothing new about a value; and a screen of "stale" before the clock
is set would be a false claim. **Would change if:** units prove constant enough to take from the
role instead, halving the subscriptions.

### D-037 The component lets malloc use PSRAM for large blocks (provisional)

**Default:** when a device has `psram:`, the component sets `CONFIG_SPIRAM_USE_MALLOC` (and clears
the psram component's `CONFIG_SPIRAM_USE_CAPS_ALLOC`), so allocations above
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KiB by default) come from PSRAM. **Why:** ui_lvgl's
`HeatRasterizer` holds a per-pixel room index in a `std::vector` (about 620 KiB for the example),
more than the ESP32-P4's internal RAM can spare, and with exceptions off a failed allocation aborts.
The heat image itself is allocated explicitly in PSRAM. **Would change if:** ui_lvgl takes an
allocator for its working set, after which this global setting can go.

### D-038 Nothing on the device blocks: scanline room maps and sliced boot and frame work (provisional)

**Question:** how much work may one call do on the panel? **What happened:** the first hardware
run with a larger config (18 rooms, a 920 x 736 map) boot-looped. `HeatRasterizer`'s constructor,
called from the component's `setup()`, tested every map pixel against every room with
`point_in_polygon()`, which also re-checked each polygon for zero area on every call, all in
double precision, which the ESP32-P4 does in software (ROM routines). It ran for 7+ seconds and
the task watchdog fired at 12 s on every boot. **Default:** (1) room ownership for any grid is
computed by scanline (`RoomScanner`, `build_room_mask()`): per row, only the edges crossing it do
double-precision work, with exactly `point_in_polygon()`'s arithmetic (shared in
`core/src/polygon_edges.h`), so the answer is identical at every centre, ties included; (2)
`validate()`'s edge-pair test rejects disjoint bounding boxes with comparisons first (exact); (3)
the device does all boot and frame work in bounded steps from `loop()`, about 20 ms of steps per
call: load (with `validate()`), mask rows, rasterizer rows (`HeatRasterizer::Deferred` and
`build_rows()`), then per frame one heat-field row (`compute_heat_field_rows()`) or eight
heat-image rows (`HeatRasterizer::render_rows()`) per step; `setup()` only subscribes and shows
"Starting". Each boot stage logs its start and its elapsed time. **Why:** a watchdog reset is the
one failure the screen cannot report, and the cost scaled with pixels x rooms x vertices, so a
bigger house would only have made it worse. **Guard:** `components/hearthmap/tests/test_cpp.py`
drives the device's pipeline on fictional configs at the schema's maximums and fails if any one
step exceeds a host cap (see the amendment below: now 2 ms per step plus a bound on each stage's
mean step scaled by the device's measured slowdown; the first version used 5 ms per step); the
pre-fix construction took 7.2 s there, 37 ms per step.
**Would change if:** a step proves too slow on the panel, in which case the step size shrinks.
**Amended (second hardware run, b68f721):** the panel booted, but the single load step (build,
whole `validate()`, colour tables) held `loop()` for 216 ms and a full heat-image repaint held
LVGL for 645 ms. Now the build is one room or pin per step (generated `build_step()`), validation
is one room or pin per step (core's `ConfigValidator`, which gives exactly `validate()`'s issues
in order), each colour table is its own step, frames render the heat image into a back buffer
and swap it in whole (so a repaint between slices never shows a half-written image), and the
swapped-in image is invalidated in 48-row bands, one per `loop()` call at most every 25 ms. The
host guard now bounds the mean step per stage scaled by the measured device slowdown (boot
about 200x, frames about 55x; bounds use 300x and 100x) and caps any single step.

### D-039 The panel never scales an RGB565 image; the host draws with the panel's formats (provisional)

**What happened:** on the panel LVGL logged "Color format 0x12 is not enabled" about three times
a second. LVGL 9.6's software transform of an RGB565 image needs both
`LV_DRAW_SW_SUPPORT_RGB565` and `LV_DRAW_SW_SUPPORT_RGB565A8`
(`src/draw/sw/lv_draw_sw_transform.c`); ESPHome 2026.11.0 enables only RGB565, ARGB8888 and
RGB888 for this device (its `lvgl` component sets them from the display format and the YAML's
images). ui_lvgl scaled the 128 px RGB565 plant photos to cover the 44 px pin and 52 px HUD
discs, so those discs were not drawn. The host never saw it: its `lv_conf.h` left every format
on. **Default:** the build generates each photo at every disc diameter ui_lvgl uses (44, 52 and
128 px; a test reads the radii from `screen.cpp`), `ImageProvider` gains
`plant_photo_sized(id, diameter)` (additive, default nullptr), and the Screen draws a pre-sized
photo centred, without a transform; the host's `lv_conf.h` now has exactly the panel's format set,
and a ui test proves a scaled RGB565 photo fails there the same way. **Rejected:** declaring
RGB565A8 to ESPHome's LVGL from the component: it would work, but every frame would then scale
photos in software on a 360 MHz core, and the pre-sized images cost about 9 KB of flash per photo
instead. **Would change if:** the UI needs a disc size chosen at run time.

### D-040 The editor repeats some `validate()` checks, as politeness (provisional)

**Question:** the editor must "validate with the real schema and show errors next to the item",
but several rules (unique ids, room references, polygon simplicity, calibration width, happy
range, band order, Fahrenheit, moisture role, map area) live in core's `validate()`, which the
browser cannot run. **Default:** the editor validates with the schema (Ajv, pinned) and repeats
those checks in `editor/src/lib/validate.ts`, blocking export on any error. Core stays the
enforcement (D-017): deleting the editor's copies changes where the author sees a problem, not
what the firmware accepts (AGENTS.md §8, layered checks). Numbered D-040 rather than the next
free number to avoid colliding with other PRs written the same night. **Would change if:** core
is compiled to WebAssembly for the editor, which would replace the copies with the real thing.

### D-041 Editor export: schema key order, no defaults written, 4-decimal coordinates (provisional)

**Default:** the exported `config.json` orders keys as the schema declares them (read from the
schema at runtime), except `entities`, which keeps the author's order; the editor never writes a
schema default the author did not set (empty fields show the default as a placeholder); edits
round map coordinates to 4 decimals (0.1 px on a 960 px map). A loaded config is exported
unchanged apart from formatting, which the round-trip test checks against `examples/flat/`.
**Why:** a config that grows defaults on every save hides what the author chose, and a stable
key order keeps diffs in the private repo small. **Would change if:** codegen wants a fully
populated config (it should apply defaults itself, D-007).

### D-042 Plant photos are exported as 128 x 128 PNG, centre-cropped (provisional)

**Default:** every image, whether attached in the inspector or opened with a project or zip, is
decoded, cut to its largest centred square and re-encoded as a 128 x 128 PNG, saved as
`photos/<photo-id>.png`; only re-encoded photos can enter the editor's store (a branded type), so
no original bytes or metadata (EXIF, GPS) reach an export, and a file that does not decode is
rejected. A folder export offers to remove `photos/<photo-id>.png` files an earlier export left
that no pin references; a referenced photo not loaded this session is kept on disk. Only photos some pin references are exported; photos are not
autosaved (they would fill `localStorage`), so after a reload the editor warns until they are
attached again. **Why:** 128 px is the largest size the UI spec draws a plant photo (the detail
overlay's 64 px radius disc); the ESPHome component converts to RGB565 at build time. **Would
change if:** the overlay grows, or the component wants a fixed RGB565 blob from the editor.

### D-043 The editor reads tools/calibrate's interchange format as that tool defines it (provisional)

**Default:** `schema/calibration.schema.json` is owned by the calibrate tool (D-029); the editor
compiles against it and tests its importer on the tool's own example output
(`tools/calibrate/examples/calibration.json`), read in place. Import applies every `status: "ok"`
sensor's calibration (all four fields) to each soil or plant pin whose `entities.moisture` matches, replacing the pin's
calibration as a whole; failed sensors are skipped with their message; confidence, warnings and
`happy_is_default` are shown, never written into the config. **Would change if:** the tool's
format changes (its schema is the contract).

### D-044 New editor projects set `map_area` to the panel layout explicitly (provisional)

**Default:** a blank project writes `canvas.map_area` = `{0, 64, 920, 736}`, the map region of
the UI spec's 1280 x 800 layout (below the 64 px top bar, left of the HUD column at x 936), rather
than relying on the whole-canvas default (D-023), which would put the map under the HUD. "Match
plan shape" keeps the area inside that region, anchored at its corner. The numbers are
`kMapRegion*` in ui_lvgl's `theme.h`, and `editor/test/defaults.test.ts` reads them from there,
so the two cannot drift. **Amended (follow-up review, issue 15):** the first default,
`{0, 0, 960, 800}`, overlapped the top bar and the HUD's left edge. **Would change if:** the
panel layout changes.

### D-045 Editor UI structure: pure `lib/`, tested; thin vanilla-TS `ui/` (provisional)

**Question:** no convention existed for a web UI in this repo (AGENTS.md §8, "building structure
where no convention is stated"). **Default:** no framework. Everything a test can check is a pure
function in `editor/src/lib/` or `editor/src/preview/scene.ts` (data in, data out, Vitest);
`editor/src/ui/` renders state with a small hyperscript helper and canvas code, and is covered by
the Playwright smoke test only. Config edits are pure functions over an immutable config, and undo
is a list of snapshots. **Why:** minimal dependencies (two at runtime: Ajv and fflate) and the
same "logic where tests reach it" rule as core (principle 2). **Would change if:** the UI grows
enough that a component framework pays for itself; `lib/` would not change.

### D-046 The UI is told when timestamps are re-stamped and when the source is offline (provisional)

**Question:** on the panel every value is re-stamped while Home Assistant is connected (D-012),
so the overlay's "seen N min ago" always read "seen just now", a claim nothing checked; and
nothing on screen said when Home Assistant was disconnected. Where does the UI learn either?
**Default:** from the embedder, in a `hm::ui::SourceStatus` passed to `Screen::update()` (a new
overload; the old three-argument form passes the host's defaults, all false). `restamped`: the
overlay shows the room only, no "seen" text. `offline`: the top bar's summary label shows
"Offline" in the warn colour in place of the house summary, until an update without it.
`heat_behind` (D-047): the heat image is dimmed. The device sets `offline` = not
`is_connected_with_state_subscription()` and `restamped` = connected (re-stamping runs exactly
then); while offline the overlay shows the honest time since the last confirmation. A change of
connection state redraws at once. **Known limit:** ESPHome 2026.11.0's
`is_connected_with_state_subscription()` is true while any API client subscribes to entity
states, and no public API says which client subscribed to Home Assistant states (the per-client
flag is private, and compiled only with wizard inputs). So a second client subscribed to states
(another Home Assistant, a script using the native API) keeps "connected" true after Home
Assistant itself disconnects: no "Offline", and values keep being re-stamped. With Home
Assistant as the only state subscriber, as in the intended setup, the flag is exact. **Why not the view model:** these describe the plumbing around
core (a connection, a re-stamping policy, an image in flight), not anything core judges from
config and readings; putting them in `ViewModel` would make core carry UI state it never uses.
**Consequences, documented in `readings.h`, `schema/README.md` and the component README:** on the
device `stale_after_s` applies only while Home Assistant is disconnected, and a probe that dies
without going `unavailable` looks fresh (issue 11). **Would change if:** Home Assistant's
`last_reported` reaches the device (the overlay could then show a real "seen" time), or the
summary is wanted alongside the warning.

### D-047 Heat images are rebuilt on a layer change at once, on new readings at most every 15 s; taps never wait (provisional)

**What happened:** on the panel one heat rebuild (field, cell colours, 920 x 736 image) takes about
1.1 s. Frames were allowed every 500 ms and compared fields exactly, so chatty sensors kept the
pipeline permanently mid-frame; UI events were applied only between frames and the screen updated
only at frame end, so a tap could wait a second or more. **Default:** `PanelPipeline` splits the
two. `update()` rebuilds the view model at once (a few ms) and the component calls it in the same
`loop()` call as a tap, a connection change, or (coalesced by `min_redraw_interval`) new readings,
then updates the screen. `heat_step()` does the heat work in the rest of each slice. A rebuild
starts at once for the first image and for a layer change (one in flight for another layer is
restarted, or dropped when the shown image is current again); for changed samples on the shown
layer at most once per `heat_refresh_interval` (new YAML option, default 15 s, 1 s to 10 min),
counted between rebuild starts; for unchanged samples never (samples are the image's whole
input, compared exactly, so the field is not computed to find out). Samples that change while a
rebuild runs are picked up when it lands, under the same interval. **What the user sees:** pins,
values, the HUD, tabs and the overlay follow every reading and tap at once; the heat image may lag
the pins by up to the interval. After a layer change the tab and the pins switch at once and the
map is dimmed to 40 % (since D-052: hidden) until the new layer's image lands (about a second on the panel), so one
layer's colours never show at full strength under another's tab. Dimming and undimming each
repaint the map whole (a style change invalidates the image); other new images still repaint in
48-row bands (D-038). The first image is dimmed the same way until it lands. **Guard:**
`components/hearthmap/tests/test_cpp.py::test_heat_rebuild_policy_and_immediate_taps` (unchanged
readings, layer-only change, readings changing within the interval, a tap and a layer change
during a rebuild, tabbing away and back); each of "always rebuild", "never record the shown
samples", "ignore a layer change", "no interval" and "update() waits for the rebuild" was seen to
fail it. **Would change if:** the panel shows the lag is confusing (shorter interval), or rebuilds
get cheap enough to drop the interval.

### D-048 A pre-flash `--check`, and one table of semantic cases for core and the editor (provisional)

**What happened:** a config the editor exported and the firmware build accepted could still be
refused at boot, because core's `validate()` first runs on the panel. Three collinear points
with inexact coordinates (`[[0.0551,0.2332],[0.0681,0.2855],[0.0941,0.3901]]`) were one: the
editor's simplicity check compared cross products with exactly zero, core's zero-area test
allows a few ulps. **Default:** (1) `hearthmap-render --check --config <json>` loads a config
through the host's JSON source and `validate()`, prints every issue with its JSON path and exits 1
on any error; the component README makes it the required step before a flash (it needs the host
build, `-DHEARTHMAP_BUILD_UI=ON`). A separate binary was not added: the render CLI already owns
the JSON source. (2) `schema/semantic-cases.json` holds configs the schema accepts and the
verdict `validate()` must give, as one change each to a base config (a room polygon or a map
area): inexact and exact collinear points, a repeated vertex, a closing vertex repeated, a
fold-back, a bow tie, a 1e-6 sliver that must be accepted, map areas off and exactly on the
canvas. Core's test reads it through a C++ table CMake generates at configure time (core's tests
stay free of a JSON parser); the editor's test reads the JSON. A case either side disagrees with
fails that side. (3) The editor now runs core's exact zero-area test (`isZeroArea()`, the same
double arithmetic) before its other polygon checks. The editor's fold-back loop, like core's, is
a second guard: every fold-back is also caught by the edge-touch test or the zero-area test, so
removing it fails no test. **Would change if:** the editor gains a WebAssembly build of core, at
which point the editor would call `validate()` itself and the table would test one
implementation.

### D-049 The house summary is core's; ui_lvgl words it (provisional)

**Question:** D-030's stopgap computed the top bar's averages in `host/src/summary.cpp`, and the
component copied that host file into the firmware build, so the device depended on `host/`
(against the layering rule, AGENTS.md §4). **Default:** core's `house_summary(config, readings,
now)` returns a `HouseSummary` of three optional means with exactly D-030's rules (fresh
temperature and humidity that feed their layers on indoor pins; fresh PM2.5 on any indoor pin; a
part with nothing fresh is empty), and `build_view_model()` fills `ViewModel::summary` with it.
ui_lvgl's `summary_text()` words it ("House 21.0°   51% RH   PM2.5 6", "House: no data"), the
same `printf` formats as before, so every golden image is unchanged. The host and the component
both use `summary_text(vm.summary)`; the build no longer copies any host file. The host's clock
formatting stays in `host/src/clock_text.cpp`. Additive to core's API: no signature changed.
**Why numbers in core and words in ui_lvgl:** averaging is logic a test can check (principle 2);
wording is presentation, like every other string on screen. **Would change if:** the summary
gains judgement (a colour for a hot house), which would be core's too.

### D-050 On the soil layer a room takes one plant's colour, against the layer's band (provisional)

**What exists:** the heat field is room-local (D-005) and the soil layer's value is thirst
(D-010), so a room with one plant is filled with that plant's thirst, coloured through the soil
layer's band (one colour table per layer). The plant's pin ring, badge and HUD entry judge the
same thirst against the plant's own band (`effective_soil_band()`: the layer band with the pin's
happy range). So the room and the pin can disagree: a plant whose happy range ends at 40 % thirst
can be badged "water soon" in a room drawn neutral, or the reverse. Two plants in one room are
blended by distance like any other samples. **Accepted for the first version:** the pin, the
badge and the HUD are the judgement; the room colour is a picture of how dry the soil is overall.
**Would change if:** the room colour should agree with the pins, by giving each soil sample a
value already normalised against its own band (issue 17), which would make the field's
domain "fraction of the way out of the happy range" rather than raw thirst.

### D-051 Pulses are stepped on one clock, and drawing cost has a budget (provisional)

**Pulses superseded by D-053** (the owner rejected the steps; sprites replace them). The rest stands.

**What happened:** on the panel the screen looked right but touch was slow. The first measured
build (c182d6b, perf log, real config, temperature layer, nobody touching) spent 76 % of all time
in LVGL refreshes, 12.8 a second of about 61 ms each, flushing 2.6 Mpx/s; the main loop ran 13
times a second. Two causes, both in `ui_lvgl/src/screen.cpp`: the room pulse animated `line_opa`
on one `lv_line` per room, and LVGL 9.6 sizes a line object from its own origin to its farthest
point, so the line sat at the map's origin and every animation frame invalidated everything from
the map's top-left corner to the room's far corner, about 60 times a second; and each danger ring
was resized every 16 ms. The host proxy measured those scenes asking for 21 to 42 Mpx/s, at a
software rendering rate on the panel of about 4.5 Mpx/s. **Default:** (1) pulses are steps on one
LVGL timer, a pure function of LVGL's tick: a ring shows radius 30, 40, 50 px at opacity 1, 0.6,
0.25, then nothing, over 1.6 s; a room outline opacity 1, 0.67, 0.35, 0.67 over 2.8 s; objects are
offset over eight slots so their steps spread over different refreshes; the timer stops when
nothing pulses. (2) A room outline is one line object per edge, positioned on its edge, so a step
invalidates strips along the walls. (3) The heat image tells LVGL it covers the area it is asked
about when it is opaque RGB565 at full opacity, so a refresh inside the map does not fill the
background first (the private cover-check struct is written: LVGL's public setter can only weaken
the verdict). (4) The component holds heat-rebuild steps for 300 ms after a press. **Trade:** the
spec's smooth ripple (radius 26 to 52, fading, 1.6 s) and eased outline (0.35 to 1, 2.8 s) become
three ring frames and four outline steps over the same periods; adjacent rooms may show different
steps at one instant. **Guard:** `host/tests/perf_proxy_test.cpp` runs the real Screen for 10
simulated seconds on the example and on a panel-shaped fictional house and bounds pixels flushed
(500 kpx/s, about 15 % of the loop at the panel's rates) and the largest single refresh (75 kpx,
about 22 ms); the previous code fails every bound by 40 to 80 times. `ui_lvgl/tests/screen_test.cpp`
checks the step counts, that no property animation runs, the strip geometry, that a cleared pulse
leaves nothing moving and stops the timer, and the cover check. **Not changed here** (D-052 takes the dimmed map and the overlay scrim): the heat repaint after
a swap; the draw buffer size. **Would change if:** the device
shows the remaining cost elsewhere, or the steps read as flicker on the glass.

### D-052 A tap's answer repaints only what it changes: no full-screen scrim, no dimmed map (provisional)

**Question:** the detail overlay drew a 72 % scrim over the whole screen, and a layer change
dimmed the whole map to 40 % until its image landed (D-047). Each is a full-area blend, and
showing or hiding it repaints the area: on the panel, about 230 ms to render and 80 ms to flush
the full screen (D-051's rates), more for a blend, so opening or closing an overlay or changing
tab could not be answered within about 150 ms. The host proxy measured 1,046 kpx redrawn to
open the overlay and 1,047 kpx to close it. **Default:** (1) the scrim is only the card's size
(card plus its "tap outside" hint) and draws nothing; its click area reaches past every screen
edge, so a tap anywhere outside the card still closes the overlay (R12 unchanged, the tap-routing
tests pass unchanged); the hint gets an opaque pill so it reads on the map. Opening or closing
now redraws about 420 kpx. (2) The heat image is 16 strip images over slices of the embedder's
buffer; while it is behind its layer every strip is hidden, so the map shows the background
under walls, labels and pins (a fill, not a blend of the image), and when it catches up the
strips come back one per 30 ms, so the new image is revealed in bands instead of one long
refresh. Swapping the buffer behind the descriptor still invalidates nothing; the strips follow
the descriptor's pointer at the next update or `invalidate_heat()`. **Trade:** the overlay no
longer darkens the screen around the card, and during a layer change (about a second) the map
shows empty rooms rather than the previous layer's colours at 40 %. Neither shows a claim the
data does not back; both look less like the approved mockup. **Guard:**
`ui_lvgl/tests/screen_test.cpp` ("veiled, then revealed in bands", "a swapped heat buffer is drawn
from the new pixels", the cover test, and the unchanged tap-routing cases);
`host/tests/perf_proxy_test.cpp` prints what each tap costs (informative, no budget yet).
Goldens overlay-plant and overlay-climate change for (1) only. **Would change if:** the owner
prefers the darkened backdrop at the cost of a slower overlay, or the device shows the card's own
repaint is still too slow.

### D-053 Pulses are pre-rendered opaque sprites; the drawing budget is a cost model (provisional)

**What happened:** 350b09a's stepped pulses fixed the temperature tab (LVGL 2 % of the time) but
the owner rejected them ("gave an impression of 3 fps"), and the soil tab with nine rings still
spent 45 % of its time in LVGL at about 32 ms per refresh for only 25 kpx: about 1 us per pixel
of anti-aliased arc, line and translucent drawing, against under 0.1 us for copying an opaque
RGB565 image and flushing it. **Default:** nothing that moves is drawn by LVGL; it is copied.
Each danger ring and each room-outline edge is an opaque RGB565 `lv_image` above the pins. What
lies under it is captured once through LVGL's own renderer (`render_area()`, following
`lv_snapshot`'s internals with an arbitrary area, twice: without the pins and with them); each
frame blends the pulse over the first capture from tables (ring alpha by distance in 1/16 px,
outline coverage per pixel), keeping pixels where the two captures differ (the pins). Rings: 20
frames over 1.6 s, radius 26 to 52 px, fading, as the spec; room outlines: 14 eased steps over
2.8 s; one LVGL timer at 16 ms shows the frame for LVGL's tick, so a slow loop drops frames
rather than queueing. Overlapping rings and outlines under a ring are composed into each tile; a
room step reaches a ring tile at that ring's next frame (at most 80 ms later), so it costs no
extra repaint. Captures happen in `Screen::prepare_step()`, one tile per call, from the
component's slices (before heat work); a change under a tile (a pin's look within its overflow,
a new heat image, the image hidden and revealed on a layer change) marks it, and until it is
recaptured it shows its old background (at most a few slices). **Budget by construction:** tile
memory is capped (`ScreenDeps::pulse_memory_bytes`, D-054; a tile needs 8 bytes per pixel, a ring
tile about 100 KB), pulses past it show only their badge or nothing; at most 4 outlines animate,
others hold a steady full-opacity outline; each ring frame repaints only the box of its old and
new radius. **The cost model** (`host/tests/perf_proxy_test.cpp`): a flushed area costs 0.85 ms,
a flushed pixel 0.077 us, a drawn pixel 0.03 us (opaque image), 0.02 (fill) or 0.55 (anti-aliased
shapes), fitted to the panel's logs; it predicts 485 ms/s for 350b09a's soil scene (measured 466)
and 434 for the first sprite version, which flushed 285 areas a second. The budget is 300 ms/s
and 33 ms per refresh; this version is 293 ms/s (200 areas a second) with refreshes of at most
11 ms. **Trades:** per ring 12.5 frames/s, not 30 (at 0.85 ms per flushed area, nine rings at 30
fps would cost 230 ms/s in fixed flush overhead alone; the screen as a whole updates about 60
times a second because rings are offset in phase); room outlines 5 steps/s; only 4 animate; a
ring is clipped at the map's edge; rings draw over room outlines (as before) but under pins. The
frame counts are constants of `PulseLayer` to raise when a cheaper flush path lands (the "hw"
work). **Guard:** the cost-model budget (350b09a fails it at 485 ms/s); screen tests that a ring
tile equals LVGL's own drawing everywhere outside the ring (removing the pin mask, or the
recapture after a heat change, each fails it), frame counts per cycle, no LVGL animation, strip
geometry, clearing. **Would change if:** the flush path gets cheaper (raise frames), or the owner
finds 12.5 frames/s per ring too slow.
**Amended after the first device run of sprites (7654a4b, real config).** The soil tab spent 55 % of
its time in LVGL at about 5 ms per repainted area, not the model's 1 ms, and the loop fell to 8
passes a second. Cause, reproduced on the host once the proxy mirrored ESPHome's rounder
(`rounder_cb` rounds every invalidated area out to its 2 px draw rounding): a rounded repaint
poked a pixel outside its opaque tile, so no object covered it and LVGL drew the background, the
heat strips, the walls, labels and pin discs under every frame. Room-outline containers also
clipped their aligned edge tiles, hiding them from LVGL's cover search. Now tiles and every
repaint inside them are aligned out to `theme::kTileAlign` (8 px, a multiple of any power-of-two
draw rounding up to 8), tiles stay inside the aligned part of the map, and containers hold their
tiles whole; the proxy's draw tasks during animation are images only. Ring tiles are the topmost
objects in the map and already carry the pins, badges and pills under them (captured with the
pins), so nothing LVGL draws sits on top of a ring tile: the condition for presenting a tile
directly to the display without an LVGL refresh (the acceleration work); where two ring tiles
overlap they hold identical pixels in the overlap. **Contrast (owner's note: a red ring on a red
room vanished):** every ring and outline now carries a dark keyline (`theme::kBackground`, 4 px
wider than the stroke, at 200/255 of its opacity) under its colour, composed in the same pass, so
it reads on any fill; golden `pulse-ring-on-red` holds the case. Badges already have a dark rim.
The perf line gains the time spent composing frames (inside LVGL's timer handler, outside the
refresh count) and capturing tiles.
**Amended again after 4d50e76 on the panel:** composing each frame at animation time cost about
16 ms per frame (24 % of all time with only the outlines animating): every step recomposed a
room's whole set of edge tiles with up to eight blended layers and an integer division per layer
per pixel. Now every frame of every tile is composed once, in `prepare_step()` (one frame per
step, after two separate capture steps), and kept; animating is pointing the descriptor at the
next frame. The composer touches only each layer's box and looks alpha up in tables (no division).
After a change under a tile its frames are recomposed while the old set plays, and the new set
replaces it whole. Rings run 50 frames and room outlines 25 steps per 1.6 s cycle (room outlines
now breathe on the rings' 1.6 s cycle, not 2.8 s, so every tile's cycle is the same and a ring
tile's frames show the outline under it exactly; trade). Every phase is a whole number of the
coarsest frames, so overlapping tiles agree. Memory: all frames are kept, within
`ScreenDeps::pulse_memory_bytes` (the component now gives three quarters of the PSRAM left after
a 6 MiB reserve and the bake); frames per cycle are divided by 2 or 4 until every pulsing object
fits, then pulses are admitted in config order while they fit. Edges are cut into pieces of at most
96 px so every tile fits the 128 x 128 a presenter accepts. At most 20 tiles are invalidated per
tick (LVGL keeps 32 areas and repaints the whole screen past that); outline containers report
that their tiles together cover a joined repaint.

### D-054 The renderer is built to port: one pixel interface, budgets from memory, regions from one table (provisional)

**Question:** a later follow-up ports the renderer to an ESP32-S3 with a 480 x 480 parallel-RGB
panel, about 8 MB of PSRAM, no PPA, no rotation (issue 27). What must hold now so that port is a
new table and an implementation, not a rewrite? **Default (owner's constraints, 2026-11-21):**
(1) every large pixel move (copies, fills, constant-alpha blends, 2x scales of the baked map
and its bands) goes through `hearthmap::ui::Blitter` (`blitter.h`, defined with the
hardware-acceleration work and copied unchanged; `SoftwareBlitter` by default, a PPA one on the
P4 later), and composing a sprite frame (a per-pixel blend from tables, which the Blitter's
operations do not express) goes through `SpriteComposer` (`sprite_composer.h`, plain C++ by
default); both are replaceable without touching callers; (2) memory budgets are parameters, not constants sized for 32 MB:
`ScreenDeps::pulse_memory_bytes` (the component passes an eighth of free PSRAM, 256 KiB to 4
MiB), with graceful degradation past them; later budgets (cached layer images, back buffers)
follow the same rule; (3) screen regions and pulse geometry come from `theme.h` (`kMapRegion*`,
`kPulseRing*`, `kRoomPulse*`), never literals in the sprite or bake code; (4) nothing in the
sprite or bake path assumes software rotation or the P4's orientation: tiles are in LVGL's
logical coordinates, and rotation stays the display driver's business; (5) the proxy's cost
model takes its rates from a table per target (`TargetRates`). **Would change if:** the port
shows a constraint missing here, in which case it is added to this list.

### D-055 Walls and labels are baked into the heat image; every layer's image is kept (provisional)

**Question:** after D-053 the map's remaining per-refresh cost was LVGL redrawing the walls (one
`lv_line` per room) and room labels over the heat image in every refreshed area, and a tab change
still meant a rebuild of about a second behind a hidden map. **Default:** (1) **bake**: at boot,
in `Screen::prepare_step()` slices of 32 rows, LVGL renders the wall and label objects alone into
memory twice, over black and over white (`render_area()` with the map given a white background
for the moment); the render over black is the drawing premultiplied by its coverage, and the
difference gives how much of the image beneath shows. `Screen::bake_rows()` lays that over rows
of a freshly rasterised heat image (one integer blend per pixel), which the device does in its
sliced heat work through `PanelPipeline`'s row hook; when the shown image carries it
(`SourceStatus::heat_baked`), the wall and label objects are hidden and LVGL never draws them
again. Rendering through LVGL was chosen over drawing glyphs and lines ourselves: it is the same
code that drew them before (fonts, kerning, ellipsis, anti-aliasing), so the baked map matches
pixel for pixel within a step of RGB565 rounding (a screen test holds every map pixel within one
step of LVGL's own drawing), and nothing about fonts is duplicated. Memory: 3 bytes per map pixel
(about 2 MB for 920 x 736), within `ScreenDeps::bake_memory_bytes`; without it the objects stay.
(2) **Kept layer images**: `PanelPipeline` takes any number of buffers; the component allocates
one per enabled layer plus one to draw into while 6 MiB of PSRAM stays free. The shown layer is
rebuilt as before; when nothing is pending, a layer not shown whose image is missing or behind
its samples is rebuilt in the background, at most once per `heat_refresh_interval`. A tab change
to a kept image shows it in the same `loop()` call as one repaint of the map (an opaque blit;
the cost model gives about 115 ms with the pins), with no hidden interval; a layer not yet kept
behaves as before (hidden, then revealed in bands). When the bake becomes ready, every kept image
is redrawn once. Pins, value pills and plant discs are not baked: their content follows every
reading, so a baked copy would force a map rebuild per reading; the pulse sprites already copy
them where pulses run. **Trades:** walls that lie on the map's edge lose their outer pixel (the
bake covers the map area only; the golden images differ there and nowhere else by more than a
rounding step); a refresh of the shown layer's image leaves pulse sprites on their old background
for the few slices until they are recaptured, while a layer change hides them until then.
**Guard:** `components/hearthmap/tests/test_cpp.py::test_layer_images_are_kept_and_shown_at_once`
(background builds, instant switches with no rebuild, the bake reaching every kept image;
removing the instant switch or the background build each fails it), the adapted heat-policy
test, and the bake fidelity screen test (skipping the blend fails it). Goldens all change for the
edge-wall pixels. **Would change if:** PSRAM gets tight (fewer kept layers is the first thing to
give), or the owner wants the edge walls whole (bake a 2 px margin).

### D-056 A tap is acknowledged at once; the detail card is cheap to draw; heat slices yield (provisional)

**Question:** on the panel a tap on a pin took 204-408 ms to show anything, because the first
thing drawn was the whole overlay card, and the card's rounded corners and 2 px border are masked
shapes over its whole 660 x 600 area (the proxy's breakdown: the border alone about 220 ms of
the 425 ms modelled to open it). **Default:** (1) a press on a pin shows a ring around it in the
next refresh (core's `hit_test()` picks the pin, as the tap will), before the tap's event has
been applied; a pressed tab pill lightens (`LV_STATE_PRESSED`). Both are a few kpx. (2) The card
is square and opaque, its stroke four 2 px fills; the overlay disc's photo is cut round over the
card colour once per photo, so the disc needs no clipping mask. Modelled opening: 425 -> 193 ms;
what remains is the disc's moisture arc and the text. (3) Heat work runs 8 ms per `loop()` once
the screen is running (20 ms for boot), so a pulse frame waits at most that long behind it.
**Trades:** the card's 20 px rounded corners and the photo's circular clipping by LVGL (now a
pre-cut circle, the same look); a rebuild takes about twice as long in wall time. **Would change
if:** the owner wants the rounded card back (a pre-rendered card background would bring it at
little cost once the overlay is slow nowhere else), or the press ring is unwelcome.
