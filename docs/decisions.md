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
`build_view_model`, `build_colour_lut`); the returning forms wrap them. `idw_power == 2` takes a
multiply-only path; other powers may use `powf`. Per-frame paths do not allocate once warmed up.
**Why:** double arithmetic is software-emulated on that chip, and a 48 x 40 field per frame is
cheap only if it stays in float and off the heap. **Would change if:** profiling on the panel says
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

**Default:** a blank project writes `canvas.map_area` = `{0, 0, 960, 800}`, the map region of the
UI spec's 1280 x 800 layout, rather than relying on the whole-canvas default (D-023), which would
put the map under the HUD. "Match plan shape" keeps the area inside 960 x 800. **Would change
if:** the panel layout changes.

### D-045 Editor UI structure: pure `lib/`, tested; thin vanilla-TS `ui/` (provisional)

**Question:** no convention existed for a web UI in this repo (AGENTS.md §8, "building structure
where no convention is stated"). **Default:** no framework. Everything a test can check is a pure
function in `editor/src/lib/` or `editor/src/preview/scene.ts` (data in, data out, Vitest);
`editor/src/ui/` renders state with a small hyperscript helper and canvas code, and is covered by
the Playwright smoke test only. Config edits are pure functions over an immutable config, and undo
is a list of snapshots. **Why:** minimal dependencies (two at runtime: Ajv and fflate) and the
same "logic where tests reach it" rule as core (principle 2). **Would change if:** the UI grows
enough that a component framework pays for itself; `lib/` would not change.
