# core/

Pure C++17, no dependencies: the logic the panel shows, as functions of their inputs. No LVGL,
no ESPHome, no clock, no I/O, no exceptions (`AGENTS.md` §4, `docs/decisions.md` D-002).

| Header | What it declares | Source |
|---|---|---|
| `geometry.h` | Map space vs canvas space, conversions, point-in-polygon, `RoomScanner` | inline / `src/geometry.cpp` |
| `config.h` | `Config` and friends mirroring the schema; `validate()` and `ConfigValidator`; the `ConfigSource` seam | inline / `src/config.cpp` |
| `readings.h` | `Reading`, `Readings`: sensor states as data | inline |
| `calibration.h` | Raw moisture -> thirst; per-plant effective soil band | `src/calibration.cpp` |
| `severity.h` | Freshness, `classify()`, `assess()` one binding | inline / `src/severity.cpp` |
| `heat_field.h` | Room mask; inverse-distance field per layer, whole or a row range at a time | `src/heat_field.cpp` |
| `colour_scale.h` | RGB565 helpers; value -> colour with a neutral comfort band; colour lookup tables | inline / `src/colour_scale.cpp` |
| `view_model.h` | `build_view_model()` (with the house summary), `heat_samples()`, `hit_test()`, `apply()` | `src/view_model.cpp` |

Every declared function is implemented and tested in `tests/` (`ctest`: `core.unit`, the
no-allocation check `core.no_alloc`, the defaults-agree-with-the-docs check, a heat-field
benchmark and smoke run). `tests/semantic_cases_test.cpp` reads `schema/semantic-cases.json`,
which the editor's tests read too (D-048). `CMakeLists.txt` compiles every public header on its
own under the strict warnings, so a header that is not self-contained fails the build.
