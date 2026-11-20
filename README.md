# hearthmap

> 🤖 AI Agents: read [AGENTS.md](AGENTS.md) instead. This README is for humans.

A wall panel that shows a heatmap of a house from Home Assistant sensors: a pin per sensor,
pulsing rooms where something is wrong, and a list of the plants that need water.

The target is an ESP32-P4 board with a 10.1" capacitive touch screen, used in landscape at
1280 x 800, running [ESPHome](https://esphome.io) with LVGL 9.6. The rendering logic is a tested
C++ library; a host renderer links the same code to produce PNGs; a browser editor places pins on
a floor plan.

## Status

Every part exists and is tested: the core library, the LVGL screen and the host renderer with its
golden images, the pin editor, the ESPHome component and the calibration tool. Each directory's
README describes what is there.

The component as of commit c9282ff has been compiled for the panel and runs on real hardware
with a private config of 18 rooms, 22 pins, 75 entities and a 920 x 736 map: boot in about 1.0 s,
first frame in 1.4 s, steady frames 16 to 23 ms, no LVGL warnings, connected to Home Assistant.
Later device changes (for example the separate heat-image budget in D-047) have been compiled
but not yet measured on the panel; `components/hearthmap/README.md` keeps the measurements and
says which commit each covers.

Nothing here is released: the firmware is built from the owner's private configuration, which
pins this repository by tag (`AGENTS.md` §7).

## How it fits together

Dependencies point inward only:

```
   components/hearthmap        host/
   (ESPHome component)         (PNG renderer, golden tests)
            \                    /
             v                  v
               ui_lvgl/  ---------> LVGL 9.6
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
at a pinned commit (and, with the UI on, LVGL, nlohmann/json and lodepng at pinned hashes).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DHEARTHMAP_BUILD_UI=ON
cmake --build build
ctest --test-dir build --output-on-failure      # core, ui_lvgl, host, golden images

# Check a config the way the panel will before flashing it (core's validate()):
build/host/hearthmap-render --check --config examples/flat/config.json

python3 -m venv .venv && .venv/bin/pip install -r schema/requirements.txt
.venv/bin/python schema/validate.py
.venv/bin/python schema/test_schema.py
```

The editor (`editor/`), the component's tests (`components/hearthmap/tests/`) and the calibration
tool (`tools/calibrate/`) each document their own commands; `AGENTS.md` §7 lists what CI runs.

## Layout

| Path | What |
|---|---|
| `core/` | The library: headers in `core/include/hearthmap/`, tests in `core/tests/` |
| `ui_lvgl/` | The LVGL object tree for one screen, driven by core's view model |
| `host/` | Headless renderer to PNG, golden images, and the pre-flash `--check` |
| `components/hearthmap/` | The ESPHome external component and its tests |
| `editor/` | The browser pin editor (static, no backend) |
| `tools/calibrate/` | Proposes per-sensor soil ranges from Home Assistant history |
| `schema/` | The config contract, its field guide, and the validator |
| `examples/` | A fictional flat and readings fixtures |
| `docs/` | Design decisions, the boundary map, review machinery |

## Licence

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE). Unless required by
applicable law or agreed to in writing, software distributed under the licence is distributed on
an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
