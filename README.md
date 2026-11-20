# hearthmap

> 🤖 AI Agents: read [AGENTS.md](AGENTS.md) instead. This README is for humans.

A wall panel that shows a heatmap of a house from Home Assistant sensors: a pin per sensor,
pulsing rooms where something is wrong, and a list of the plants that need water.

The target is an ESP32-P4 board with a 10.1" capacitive touch screen, used in landscape at
1280 x 800, running [ESPHome](https://esphome.io) with LVGL 9.5. The rendering logic is a tested
C++ library; a host renderer links the same code to produce PNGs; a browser editor places pins on
a floor plan.

## Status

Early. This is the scaffold: build, tests, CI, the config schema with a fictional example, and the
core library's public headers. Behaviour lands in this order:

| PR | What |
|---|---|
| 1 | Scaffold, schema, example, core headers (this) |
| 2 | core: config validation, calibration, severity |
| 3 | core: geometry, heat field, colour scale |
| 4 | core: view model, HUD selection, hit-testing |
| 5 | LVGL UI and host renderer, golden images |
| 6 | Pin editor (static web app) |
| 7 | ESPHome external component |
| 8 | Calibration tool |

## How it fits together

Dependencies point inward only:

```
   components/hearthmap        host/
   (ESPHome component)         (PNG renderer, golden tests)
            \                    /
             v                  v
               ui_lvgl/  ---------> LVGL 9.5
               (object tree, diffing)
                    |
                    v
                  core/            pure C++17, no dependencies:
                                   config, calibration, severity,
                                   heat field, colours, view model

   editor/  --exports-->  config JSON  <--validates--  schema/hearthmap.schema.json
   tools/calibrate/  --writes dry/wet ranges into-->  config JSON
```

Core takes everything as data (the config, the readings, the current time, the UI state) and
returns what the screen should show. Nothing in it knows about LVGL, ESPHome, a clock or a file,
so the same code runs in unit tests, on a laptop and on the panel.

A house's real config (floor plan, entity ids, photos) stays private to its owner and is compiled
into their firmware elsewhere. This repository only ever holds the fictional flat in
[`examples/flat/`](examples/flat/).

## Build and test

Requires CMake >= 3.21, a C++17 compiler and Python >= 3.10. The first configure fetches doctest
at a pinned commit.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

python3 -m venv .venv && .venv/bin/pip install -r schema/requirements.txt
.venv/bin/python schema/validate.py
.venv/bin/python schema/test_schema.py
```

## Layout

| Path | What |
|---|---|
| `core/` | The library: headers in `core/include/hearthmap/`, tests in `core/tests/` |
| `ui_lvgl/`, `host/`, `components/hearthmap/`, `editor/`, `tools/calibrate/` | Later PRs; each has a README saying what lands there |
| `schema/` | The config contract, its field guide, and the validator |
| `examples/` | A fictional flat and readings fixtures |
| `docs/` | Design decisions, the boundary map, review machinery |

## Licence

No licence has been chosen yet, so no reuse licence is granted for now.
