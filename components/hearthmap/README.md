# components/hearthmap/

The ESPHome external component that puts hearthmap on a panel. Verified against ESPHome
**2026.11.0** (source at that tag) with its bundled LVGL 9.6.0 and the native ESP-IDF toolchain.

```yaml
external_components:
  - source: github://abradner/hearthmap@<tag>   # or: {type: local, path: <checkout>/components}
    components: [hearthmap]

time:
  - platform: homeassistant
    id: ha_time

lvgl:
  # ...displays, touchscreens, rotation...
  widgets: []          # hearthmap draws the active screen itself

hearthmap:
  config: hearthmap.json   # schema-valid config (schema/hearthmap.schema.json)
  photos: photos           # <photo id>.png for each plant pin that names a photo
  time_id: ha_time
  # lvgl_id: ...            only needed with more than one lvgl instance
  # refresh_interval: 30s   re-judge freshness and redraw even when nothing changed (1s..10min)
  # min_redraw_interval: 500ms  coalesce bursts of state updates (50ms..10s)
  # heat_refresh_interval: 15s  how often changed readings may rebuild the heat image (1s..10min)
```

A full example device is `examples/flat/esphome/flat-panel.yaml`.

## What happens at build time

`hearthmap_codegen.py` (pure Python, tested in `tests/`):

1. **Schema check** while ESPHome validates the YAML, so `esphome config` already fails on a bad
   config. ESPHome's image has no `jsonschema`, so the component carries a small checker for
   exactly the keywords `schema/hearthmap.schema.json` uses; it raises on any keyword it does not
   implement instead of skipping it, and its tests run every negative control of
   `schema/test_schema.py` through it and compare its verdicts with `jsonschema`'s.
2. **Photos**: every photo id a pin names must exist as `<photos>/<id>.png`, or the build fails
   (the editor exports the folder with the config, so a missing file is a broken export). Each is
   centre-cropped, flattened onto the plant disc colour, resized to **128 x 128** (the overlay
   disc), 52 x 52 (HUD card) and 44 x 44 (pin), each stored as RGB565 little-endian
   (`LV_COLOR_FORMAT_RGB565`), about 41 KiB per photo in flash. The panel's LVGL cannot scale
   RGB565 images (D-039), so ui_lvgl takes each size through `ImageProvider::plant_photo_sized()`
   and draws it unscaled. Plant pins with no photo id get ui_lvgl's own placeholder leaf.
3. **Tables**: C++ that builds a `hearthmap::Config` (`hearthmap_gen::GeneratedConfigSource`,
   core's `ConfigSource` seam). Scalar defaults come from the schema's `default` keywords and an
   omitted `map_area` is the whole canvas (D-023); band defaults are left to core's
   `default_band()`, so neither set of numbers is copied here (D-007). `LayerConfig::present`
   gets a bit for exactly the band fields the JSON wrote, `null` included (D-011). Bindings are
   emitted in `Role` order whatever the JSON key order (D-019).
4. **Library copy** (below).

**Before every flash, run the check** (D-048). The build checks the schema only, so a config
that the editor and the build accept can still be refused at boot by core's `validate()`, which
first runs on the panel. Build the host tools once and check the exact file the firmware will
compile:

```sh
cmake -S . -B build -DHEARTHMAP_BUILD_UI=ON && cmake --build build --target hearthmap-render
build/host/hearthmap-render --check --config <path to hearthmap.json>
```

It loads the config through the host's JSON source and core's `validate()`, prints every issue
with its JSON path, and exits non-zero on any error ("refused: the panel would not draw this
config"); "ok: no errors, N warnings" means the panel will accept it.

**Semantic rules are not re-implemented in Python** (D-017). The firmware runs core's
`validate()` at boot; on any Error it draws the first issues on screen, logs every issue with its
JSON-pointer path, sets the component's error status and renders nothing else. `tests/test_cpp.py`
links the generated tables with core's real `validate()` on the host, so a semantically broken
config is proven to be refused, and the example proven to load.

## At run time

- One Home Assistant state subscription per distinct bound entity (plus one for its
  `unit_of_measurement` attribute), straight from the generated table: no per-entity YAML.
  `predicted_dry_at` bindings are not subscribed (core ignores that role in v1).
- States go through `readings_ingest.h`: `unavailable` and `unknown` map to their reading states,
  a finite decimal number to a value, anything else to Unknown.
- **Ruling R8** (D-012): while the API connection with a state subscription is up, every reading
  holding a value is re-stamped to now on each view rebuild; when it drops, re-stamping stops and
  readings age into TooOld honestly. Two consequences (D-046):
  - **`stale_after_s` applies only while Home Assistant is disconnected.** While connected every
    value is "confirmed" each rebuild, so it never ages. A probe that dies without Home Assistant
    marking it `unavailable` keeps its last value and looks fresh (issue 11).
  - The detail overlay shows **no "seen N min ago"** while connected (it would always read "just
    now"), only the room. While disconnected it shows the honest time since the last confirmation.
- **Offline**: while that connection is down, the top bar shows "Offline" in the warn colour in
  place of the house summary; it clears on reconnect. A change of connection state redraws at
  once. ESPHome reports "a client with a state subscription", not "Home Assistant" in
  particular, so a second native-API client subscribed to states hides a Home Assistant
  disconnect (D-046, known limit).
- **Time**: until the `time_id` source is valid the screen says "Waiting for time" and nothing is
  judged, rather than marking every sensor stale.
- **Nothing blocks** (D-038). `setup()` only subscribes, takes over LVGL's log output (to count
  warnings) and shows "Starting". All heavy work is `PanelPipeline`'s (`panel_pipeline.h`, pure
  C++, host-tested), run from `loop()` in slices of about 20 ms of small steps; then `loop()`
  returns so the watchdog, Wi-Fi, the API and LVGL run:
  - boot: build (the generated tables, one room or pin per step), validate (core's
    `ConfigValidator`, one room or pin per step), prepare (one colour table per step, then the
    room scanner), mask rows and rasterizer rows (4 per step), then the `Screen`. Each stage logs
    `boot: <stage> started (N units)` and `boot: <stage> done in N ms over K loop slices (longest
    step X ms)` at INFO, then `boot: complete in N ms`;
  - view (D-047): on a state change (at most every `min_redraw_interval`), at least every
    `refresh_interval`, on a change of connection state, and in the same `loop()` call as a
    touch: core's view model and the screen update, a few milliseconds. Pins, values, the HUD,
    the tabs and the overlay always show the latest readings and the latest tap, even while a
    heat image is being rebuilt;
  - heat image (D-047): rebuilt in the rest of each slice, one heat-field row per step, then
    eight image rows per step into a back buffer, swapped in whole when complete. A rebuild
    starts at once for the first image and for a layer change (restarting one in flight for
    another layer); for changed readings on the same layer at most once per
    `heat_refresh_interval`, so chatty sensors cannot keep the pipeline busy; for unchanged
    readings never. Until a layer change's image lands (about a second on the panel) the map is
    **dimmed** to 40 %, so one layer's colours never show at full strength under another's tab;
    undimming repaints the map whole. Otherwise a new image is repainted in 48-row bands, one per
    `loop()` call at most every 25 ms, instead of one long LVGL refresh. The first image logs its
    time at INFO; later ones log at DEBUG with the running count of LVGL warnings and errors.
- LVGL's image cache is off in ESPHome's build (`LV_CACHE_DEF_SIZE` 0), so pointing the heat
  image descriptor at the other buffer is enough for LVGL to draw the new pixels.

## How core and ui_lvgl reach the firmware

ESPHome copies only the top-level files of a component directory into the build, and
`core/` and `ui_lvgl/` sit outside it. **Mechanism chosen: a build-time copy by the component's
Python.** For both a `github://` source (ESPHome clones the whole repository, depth 1, into its
data directory: `esphome/git.py`) and a local path, the rest of the checkout sits next to the
component, so `to_code` copies each library's `include/` tree to the root of the build's `src/`
(ESPHome's generated `src/CMakeLists.txt` globs `src/**` recursively and has `.` on the include
path: `esphome/build_gen/espidf.py`) and its `src/` tree to `src/hearthmap_lib/<lib>/`, together
with the generated files in `src/hearthmap_gen/`. Files are written only when changed, and stale
files in those directories are removed. Core's sources also get `-ffp-contract=off`, as on the
host, through `esp32.add_src_cmake`.

Properties: the library is pinned by the same ref as the component (one `@<tag>`, no second ref
to drift); no network access beyond ESPHome's own clone; a local path builds the working tree as
it is.

Rejected:

- **`add_idf_component(name=..., repo=..., ref=..., path=...)`** (`esphome/components/esp32/__init__.py`):
  the IDF component manager would fetch the repository a second time (network the build does not
  otherwise need), at a ref the component cannot know from its own source, so a local-path build
  would still compile the remote library; and core's `CMakeLists.txt` is a plain host build, not
  an IDF component, so it would need a second build description to keep in step.
  `add_idf_component(path=<checkout>)` avoids the fetch but still needs that IDF-specific
  `CMakeLists.txt` in `core/` and `ui_lvgl/`, which are not this PR's files.
- **`cg.add_library` with a git URL**: a PlatformIO library, which on the ESP-IDF toolchain is
  converted through PlatformIO's resolver; same second fetch and ref problem.
- **A vendored copy in `components/hearthmap/` with a CI check that it matches**: two copies of
  core in one repository, kept equal by a check rather than by construction.

Known limit: if `hearthmap:` is removed from a device's YAML, the copied directories stay in that
device's build tree until `esphome clean`.

## UI layer

The glue drives ui_lvgl's public API directly (`ui_lvgl/include/hearthmap/ui/`), composed the
same way as the host renderer (`host/src/app.cpp`): a `Screen` on the LVGL instance's active
screen with the built-in Montserrat fonts (the component declares the seven sizes and the
`label`, `line`, `image` and `arc` widgets to ESPHome's LVGL, which compiles only what a build
declares), a `HeatRasterizer` writing into a heat image buffer the glue allocates in PSRAM,
`colour_cells()` through core's `ColourLut` with the spec palette, the component as the
`EventSink` (events are queued and applied with core's `apply()` in `loop()`, as `ports.h`
asks) and the generated photos as the `ImageProvider`. The heat image is redrawn only when the
active layer or the heat samples changed (D-047). The glue tells the `Screen` what the view model
does not carry through `hm::ui::SourceStatus`: offline, re-stamped timestamps, and a heat image
behind the active layer (D-046). The top bar's house summary is core's `ViewModel::summary`
worded by ui_lvgl's `summary_text()`, the same path as the host (D-049).

## Memory and cost (example config, 1280 x 800, measured from the build and the headers)

| Item | Size | Where |
|---|---|---|
| Heat image, map area 864 x 720 RGB565, two buffers (front and back) | 2 x 1,244,160 B | PSRAM (`RAMAllocator`) |
| `HeatRasterizer` working set (per-pixel room index + tables) | about 643,000 B | PSRAM via malloc (D-037) |
| Photos: 5 photos x (128, 52, 44 px) RGB565 | 210,240 B | flash (rodata) |
| `HeatField` x 2 (48 x 40 x 8 B), `RoomMask`, cell colours, 4 colour tables | about 37,000 B | heap |
| LVGL draw buffer at the default `buffer_size: 100%` | 2,048,000 B | PSRAM (ESPHome's lvgl) |
| Software rotation buffer (800 x 1280 panel shown landscape) | up to 2,048,000 B | ESPHome's lvgl / display driver |

The board has 32 MiB of PSRAM, so the total (about 6 MiB) fits with room to spare; internal RAM
use after the build was 15.6 % of 576 KiB (static only: the heap is not measured until it runs).

## Cost

History: the first hardware run boot-looped on the task watchdog (a per-pixel
`point_in_polygon()` loop inside `setup()`); the second booted, with a 216 ms load step and a
645 ms LVGL repaint (D-038). `tests/test_cpp.py` drives the device's pipeline on the host (-O2,
best of three boots) on fictional configs and checks, per stage, that the mean step scaled by
the device's measured slowdown stays under 50 ms, and caps any single step:

| Fictional config | Boot, host | Mean boot step (worst stage) | Frame, host |
|---|---|---|---|
| Panel-sized: 18 rooms, 22 pins, 131 x 105 grid, 920 x 736 map | 1.3 ms | 5 us (raster) | about 15 ms |
| Schema maximum, 64 comb rooms of 64 vertices, 256 pins, 256 x 256 grid, 1280 x 800 map | 20 ms | 60-70 us (mask) | about 35 ms |
| Schema maximum, all 256 pins in one room | 20 ms | 60 us (mask) | about 60 ms |

Measured on the panel (ESP32-P4 rev v1.3 at 360 MHz, 2d81b5a, a private config of the
panel-sized shape: 18 rooms, 22 pins, 75 entities, a 920 x 736 map). These numbers cover that
commit and the next one measured, c9282ff (the last component commit before the follow-up
review), which booted in 1.02 s, drew its first frame in 1.4 s and then steady frames of 16 to
23 ms, with no LVGL warnings and Home Assistant connected. The separate view and heat paths
(D-047) and the SourceStatus wiring (D-046) came after and have been compiled for the panel but
not measured on it:

| Boot stage | Wall time | Longest single step |
|---|---|---|
| build | 451 ms over 1 slice (mostly waiting for the next `loop()` call) | 0.25 ms |
| validate | 6 ms | 0.68 ms |
| prepare (colour tables, scanner) | 10 ms | 2.5 ms |
| mask | 70 ms | 13.0 ms |
| raster | 238 ms over 6 slices | 0.84 ms |
| screen (two 1,354,240 B heat buffers; rasterizer 710,747 B) | 233 ms, one call | - |
| boot complete | 1,011 ms | - |

Frames: first frame 1,446 ms, a frame with a new heat image 1,100 ms (both spread over many
slices), steady frames 15-21 ms. `LVGL warnings 0, errors 0` throughout. ESPHome still warns
once each at boot for the screen build (hearthmap, 249 ms) and two LVGL refreshes (224 ms and
139 ms).

