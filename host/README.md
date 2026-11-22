# host/

The host renderer: the same `ui_lvgl` code on a headless LVGL display, plus the host's
`ConfigSource` (JSON parsing lives here, never in core) and the golden-image tests.

```sh
cmake -S . -B build -DHEARTHMAP_BUILD_UI=ON
cmake --build build
build/host/hearthmap-render --config examples/flat/config.json \
  --readings examples/flat/readings/thirsty-many.json --layer soil --out frame.png
```

Options: `--layer temperature|humidity|soil|air`, `--tap x,y` (repeatable, canvas pixels,
delivered through LVGL's input pipeline and applied with core's `apply()` before the next one),
`--time-ms N` (the LVGL clock when the frame is drawn, so pulses are at that phase), `--photos
<dir>` (plant photos as `<dir>/<id>.png`), `--utc-offset-min N` (clock text), `--timing` (prints
render times and buffer sizes). `now` is the readings fixture's `now`; timestamps are used as
given.

`hearthmap-render --check --config <json>` only loads the config (the JSON source, then core's
`validate()`), prints each issue with its JSON path and exits 1 on any error: the step before
flashing a panel (D-048, `components/hearthmap/README.md`).

## Dependencies

All fetched by CMake at a pinned content hash on first configure (`cmake/`):

| What | Version | Licence |
|---|---|---|
| LVGL | 9.6.0, commit `80ca777e` | MIT |
| nlohmann/json | 3.12.0 | MIT |
| lodepng | commit `7b2893e3` | zlib |

The default build needs no SDL and opens no window. There is no SDL target yet.

## Golden images

`host/tests/golden/*.png` are frames of the fictional flat, rendered at `--time-ms 400`:

| Golden | Fixture | Arguments |
|---|---|---|
| `all-ok` | all-ok | |
| `thirsty-many` | thirsty-many | `--layer soil` |
| `stale-sensors` | stale-sensors | |
| `humid-bathroom` | humid-bathroom | `--layer humidity` |
| `overlay-plant` | thirsty-many | `--layer soil --tap 580,119` (basil) |
| `overlay-climate` | all-ok | `--tap 220,292` (living-room climate pin) |
| `layer-temperature`, `layer-humidity`, `layer-air` | thirsty-many | `--layer ...` |
| `layer-soil` | all-ok | `--layer soil` |
| `rotated-90`, `rotated-180`, `rotated-270` | thirsty-many | `--layer soil`, on the example turned by `canvas.map_rotation` (D-080) |
| `rotated-90-overlay-plant` | thirsty-many | `--layer soil --tap 604,526` (basil, where the turned map draws it) |

The rotated cases render configs CMake writes at configure time from `examples/flat/config.json`
itself (`build/host/tests/rotated/flat-<deg>.json`), changing only `map_rotation` and
`map_area`: at 90 and 270 the area is 600 x 720 at the example's corner, the plan's 864 x 720
proportions swapped; at 180 it is the example's own.

`ctest` renders each and compares it with the committed PNG **exactly**. On a mismatch the actual
image and a diff (differences in magenta over the dimmed actual frame) are left in
`build/golden-out/`; CI uploads that directory as an artifact. `golden.comparison_can_fail`
plants a one-pixel change in a golden and requires the comparison to report exactly that pixel.

Exactness across compilers holds because nothing in the pipeline depends on floating-point
rounding that differs between them: `-ffp-contract=off` everywhere, core's field is
bit-reproducible (D-024), colours come from a table, and the rasterizer is integer-only.

### Changing a golden

```sh
cmake --build build --target update-goldens
git diff --stat host/tests/golden/   # then open every changed PNG and look at it
```

A commit that changes a golden must say, in words a reviewer can check against the picture, what
changed on screen and why (for example "pin values get a dark backing so a red value reads on red
heat"). "Regenerated goldens" is not a reason. Never regenerate to make a red CI green without
first reading the diff image: the golden is the claim, and a changed pixel is a changed claim.
