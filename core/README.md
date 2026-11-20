# core/

Pure C++17, no dependencies: the logic the panel shows, as functions of their inputs. No LVGL,
no ESPHome, no clock, no I/O, no exceptions (`AGENTS.md` §4, `docs/decisions.md` D-002).

| Header | What it declares | Implemented in |
|---|---|---|
| `geometry.h` | Map space vs canvas space, conversions, point-in-polygon | inline / PR 3 |
| `config.h` | `Config` and friends mirroring the schema; `validate()`; the `ConfigSource` seam | inline / PR 2 |
| `readings.h` | `Reading`, `Readings`: sensor states as data | inline |
| `calibration.h` | Raw moisture -> thirst; per-plant effective soil band | PR 2 |
| `severity.h` | Freshness, `classify()`, `assess()` one binding | inline / PR 2 |
| `heat_field.h` | Room mask; inverse-distance field per layer | PR 3 |
| `colour_scale.h` | RGB565 helpers; value -> colour with a neutral comfort band | inline / PR 3 |
| `view_model.h` | `build_view_model()`, `heat_samples()`, `hit_test()`, `apply()` | PR 4 |

PR 1 ships the headers with full contracts and the inline helpers with tests
(`tests/inline_helpers_test.cpp`); declared functions are implemented, and tested, by the PR
listed. `CMakeLists.txt` compiles every public header on its own under the strict warnings, so a
header that is not self-contained fails the build.
