"""C++ checks run from pytest: the device glue's pure parts, and generated tables against core.

Each test writes a small C++ program, compiles it with the host compiler under core's warning
flags, and runs it. The generated-config test links core's real validate(), so it proves the
generator's output (defaults, present bits, bindings) is a Config core accepts, and that a
schema-valid but semantically broken config is refused at load time, which is what the firmware
does at boot.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
CORE_SRC = REPO / "core" / "src"
FLAGS = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti",
         "-ffp-contract=off"]


def _cxx() -> str:
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.fail("no C++ compiler found (set CXX)")
    return cxx


def _core_sources() -> list[str]:
    srcs = sorted(str(p) for p in CORE_SRC.glob("*.cpp")) if CORE_SRC.is_dir() else []
    if not srcs:
        if os.environ.get("HEARTHMAP_REQUIRE_CORE") == "1":
            pytest.fail("core/src has no implementation, and HEARTHMAP_REQUIRE_CORE=1")
        pytest.skip("core/src not present on this branch yet")
    return srcs


def _build_and_run(tmp_path: Path, sources: list[str], includes: list[Path]) -> str:
    exe = tmp_path / "t"
    cmd = [_cxx(), *FLAGS, *(f"-I{i}" for i in includes), *sources, "-o", str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr[-4000:]
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stdout[-4000:] + run.stderr[-4000:]
    return run.stdout


GLUE_TEST = r"""
#include <cmath>
#include <cstdio>
#include "readings_ingest.h"
using namespace hearthmap;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void value(const char* s, double v) {
  Reading r = hearthmap_device::parse_state(s, 100);
  if (r.state != ReadingState::Value || r.value != v || r.last_updated != 100) {
    std::printf("FAIL value '%s'\n", s); ++failures;
  }
}
static void state(const char* s, ReadingState want) {
  Reading r = hearthmap_device::parse_state(s, 100);
  if (r.state != want) { std::printf("FAIL state '%s'\n", s); ++failures; }
}

int main() {
  value("21.5", 21.5); value(" 7 ", 7.0); value("-0.5e2", -50.0); value("+3", 3.0);
  value("0", 0.0); value(".5", 0.5); value("5.", 5.0);
  state("unavailable", ReadingState::Unavailable);
  state("unknown", ReadingState::Unknown);
  for (const char* bad : {"", " ", "nan", "NaN", "inf", "-inf", "1e999", "21.5 C", "0x10", "1e",
                          ".", "-", "1,5", "Unavailable", "on"}) {
    state(bad, ReadingState::Unknown);
  }

  Readings rs;
  hearthmap_device::apply_unit(rs, "sensor.a", "\xC2\xB0" "C");
  CHECK(rs.find("sensor.a")->state == ReadingState::Unknown);
  CHECK(rs.find("sensor.a")->last_updated == 0);
  hearthmap_device::apply_state(rs, "sensor.a", "20.0", 50);
  CHECK(rs.find("sensor.a")->value == 20.0);
  CHECK(rs.find("sensor.a")->unit == "\xC2\xB0" "C");  // unit survives a state update
  hearthmap_device::apply_state(rs, "sensor.b", "unavailable", 50);
  CHECK(rs.find("sensor.b")->state == ReadingState::Unavailable);

  // R8: re-stamp Value readings only, and only while connected.
  CHECK(hearthmap_device::restamp(rs, false, 999) == 0);
  CHECK(rs.find("sensor.a")->last_updated == 50);
  CHECK(hearthmap_device::restamp(rs, true, 999) == 1);
  CHECK(rs.find("sensor.a")->last_updated == 999);
  CHECK(rs.find("sensor.b")->last_updated == 50);
  std::printf("glue checks done, %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
"""


def test_readings_ingest(tmp_path):
    (tmp_path / "glue_test.cpp").write_text(GLUE_TEST)
    out = _build_and_run(
        tmp_path,
        [str(tmp_path / "glue_test.cpp"), str(COMPONENT / "readings_ingest.cpp")],
        [COMPONENT, REPO / "core" / "include"],
    )
    assert "0 failures" in out


LOAD_TEST = r"""
#include <cstdio>
#include "hearthmap_gen/generated_config.h"
using namespace hearthmap;
int main() {
  ConfigLoadResult r = hearthmap_gen::GeneratedConfigSource{}.load();
  for (const ConfigIssue& i : r.issues)
    std::printf("%s %s: %s\n", i.severity == IssueSeverity::Error ? "ERROR" : "warning",
                i.path.c_str(), i.message.c_str());
  if (!r.config) { std::printf("REFUSED\n"); return 0; }
  const Config& c = *r.config;
  std::printf("LOADED rooms=%zu pins=%zu map=%d,%d,%d,%d hud=%d present_air=%u entities=%zu\n",
              c.rooms.size(), c.pins.size(), c.canvas.map_area.x, c.canvas.map_area.y,
              c.canvas.map_area.width, c.canvas.map_area.height, c.hud.max_items,
              unsigned(c.layer(Layer::Air).present), hearthmap_gen::kSubscribedEntityCount);
  std::printf("air.warn_above=%g air.scale_min=%g\n", *c.layer(Layer::Air).band.warn_above,
              c.layer(Layer::Air).band.scale_min);
  return 0;
}
"""


def _load(gen, schema, doc, tmp_path) -> str:
    core = _core_sources()
    tmp_path.mkdir(parents=True, exist_ok=True)
    out = gen.emit_config_source(doc, schema, "test.json")
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "load_test.cpp").write_text(LOAD_TEST)
    return _build_and_run(
        tmp_path,
        [str(tmp_path / "load_test.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"), *core],
        [tmp_path, REPO / "core" / "include"],
    )


def test_generated_example_is_accepted_by_core(gen, schema, example, tmp_path):
    out = _load(gen, schema, example, tmp_path)
    assert "LOADED rooms=7 pins=18 map=40,76,864,720 hud=4 present_air=8 entities=33" in out, out
    # The written field took effect; the unwritten one came from core's default_band() (R9: 350).
    assert "air.warn_above=1000 air.scale_min=350" in out, out
    assert "ERROR" not in out


def test_schema_valid_but_semantically_broken_config_is_refused(gen, schema, example, tmp_path):
    example["rooms"][1]["id"] = example["rooms"][0]["id"]  # duplicate room id: validate() only
    assert gen.check_schema(schema, example) == []  # the schema cannot see it
    out = _load(gen, schema, example, tmp_path)
    assert "REFUSED" in out, out
    assert "ERROR /rooms/1" in out, out


def test_fahrenheit_with_defaulted_band_is_refused_by_core(gen, schema, minimal, tmp_path):
    # Present bits are what lets validate() see this: if the generator marked defaulted fields
    # present, core would take the Celsius defaults as Fahrenheit. The one written value keeps
    # the band ordered either way, so only the Fahrenheit rule can refuse it.
    minimal["layers"] = {"temperature": {"comfort_min": 20}}
    celsius = _load(gen, schema, minimal, tmp_path / "c")
    assert "LOADED" in celsius, celsius  # positive control: the same band is fine in Celsius
    minimal["temperature_unit"] = "F"
    out = _load(gen, schema, minimal, tmp_path / "f")
    assert "REFUSED" in out, out
    assert "ERROR /layers/temperature:" in out, out


# ---------------------------------------------------------------------------------------------
# The boot and frame pipeline at the schema's maximums (docs/decisions.md D-038).
#
# The first hardware run boot-looped: building the heat image's per-pixel room map called
# point_in_polygon() for every pixel and room, in software double precision on the ESP32-P4,
# inside setup(), for 7+ seconds, until the task watchdog fired. This test drives the same
# PanelPipeline the device does, on fictional configs at the schema's limits (64 rooms of 64
# vertices, 256 pins, a 256 x 256 grid, a 1280 x 800 map), and fails when any single step, the
# unit of work the device runs between yields, is slower than a host budget. The budget is set so
# that, scaled by the device's slowdown on this kind of code (README.md, "Cost"), a step stays
# well under the device's slice; the old per-pixel construction misses it by orders of magnitude.
# ---------------------------------------------------------------------------------------------

STEP_BUDGET_MS = 5.0  # per step, host, -O2


def _comb(x0: float, x1: float, teeth: int) -> list[list[float]]:
    """A comb inside [x0, x1]: a bar along the bottom and `teeth` teeth reaching up. 16 teeth is
    64 vertices (the schema's maximum), and every row through the teeth crosses 32 edges."""
    w = (x1 - x0) / (2 * teeth - 1)
    pts = [[x0, 1.0]]
    for t in range(teeth):
        left = x0 + 2 * t * w
        pts += [[left, 0.02], [left + w, 0.02]]
        if t + 1 < teeth:
            pts += [[left + w, 0.9], [left + 2 * w, 0.9]]
    pts.append([x1, 1.0])
    return [[round(x, 12), y] for x, y in pts]


def _max_config(one_big_room: bool) -> dict:
    rooms = []
    if one_big_room:  # every pin in one room: the heat field's worst case (256 samples per cell)
        rooms.append({"id": "hall", "name": "Hall", "polygon": [[0, 0], [1, 0], [1, 1], [0, 1]]})
    while len(rooms) < 64:
        i = len(rooms)
        rooms.append({"id": f"room_{i}", "name": f"Room {i}", "polygon": _comb(i / 64, (i + 1) / 64, 16)})
    pins = []
    for k in range(256):
        if one_big_room:
            room, x, y = "hall", (k % 16 + 0.5) / 16, (k // 16 + 0.5) / 16
        else:
            r = k // 4
            room, x, y = f"room_{r}", (r + (k % 4 + 0.5) / 4) / 64, 0.97
        pins.append({"id": f"pin_{k}", "kind": "temperature", "name": f"Sensor {k}",
                     "position": [x, y], "room": room,
                     "entities": {"temperature": f"sensor.example_max_{k}"}})
    return {
        "version": 1,
        "canvas": {"width": 1280, "height": 800, "orientation": "landscape",
                   "map_area": {"x": 0, "y": 0, "width": 1280, "height": 800}},
        "rooms": rooms, "pins": pins,
        "heat": {"grid_cols": 256, "grid_rows": 256, "idw_power": 3},
        "hud": {"max_items": 20},
    }


PIPELINE_DRIVER = r"""
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "hearthmap_gen/generated_config.h"
#include "panel_pipeline.h"
using namespace hearthmap;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
int main() {
  hearthmap_gen::GeneratedConfigSource source;
  hearthmap_device::PanelPipeline p(source, Palette{});
  double worst = 0, total = 0;
  std::string worst_stage;
  int steps = 0;
  auto t_boot = Clock::now();
  while (p.stage() != hearthmap_device::PanelPipeline::Stage::Ready &&
         p.stage() != hearthmap_device::PanelPipeline::Stage::Refused) {
    const char* name = hearthmap_device::PanelPipeline::stage_name(p.stage());
    auto a = Clock::now();
    p.boot_step(4);  // the device's kBootRowsPerStep
    double d = ms(a, Clock::now());
    if (d > worst) { worst = d; worst_stage = name; }
    ++steps;
  }
  total = ms(t_boot, Clock::now());
  if (p.stage() != hearthmap_device::PanelPipeline::Stage::Ready) {
    for (const auto& i : p.issues()) std::printf("ISSUE %s: %s\n", i.path.c_str(), i.message.c_str());
    std::printf("REFUSED\n");
    return 1;
  }
  std::printf("BOOT steps=%d total_ms=%.2f worst_step_ms=%.3f worst_stage=%s\n", steps, total, worst,
              worst_stage.c_str());
  std::vector<Rgb565> heat(p.rasterizer().pixel_count());
  p.set_heat_buffer(heat.data());
  Readings readings;
  for (std::size_t i = 0; i < hearthmap_gen::kSubscribedEntityCount; ++i) {
    Reading r; r.state = ReadingState::Value; r.value = 18.0 + double(i % 9); r.last_updated = 1000;
    readings.by_entity.emplace(hearthmap_gen::kSubscribedEntities[i].entity_id, r);
  }
  double fworst = 0, ftotal = 0; int fsteps = 0;
  for (int frame = 0; frame < 3; ++frame) {
    auto a = Clock::now();
    p.begin_frame(readings, 1000, initial_ui_state(p.config()));
    double d = ms(a, Clock::now()); if (d > fworst) fworst = d; ++fsteps;
    bool done = false;
    while (!done) {
      auto b = Clock::now();
      done = p.frame_step(1);  // the device's kFieldRowsPerStep
      double e = ms(b, Clock::now()); if (e > fworst) fworst = e; ++fsteps;
    }
    ftotal += ms(a, Clock::now());
    readings.by_entity.begin()->second.value += 1.0;  // force a field change, so a redraw
  }
  std::printf("FRAME steps=%d total_ms=%.2f worst_step_ms=%.3f redrawn=%d\n", fsteps, ftotal, fworst,
              int(p.heat_redrawn()));
  return 0;
}
"""


def _pipeline(gen, schema, doc, tmp_path) -> dict:
    core = _core_sources()
    assert gen.check_schema(schema, doc) == [], gen.check_schema(schema, doc)[:3]
    out = gen.emit_config_source(doc, schema, "max.json")
    (tmp_path / "hearthmap_gen").mkdir(parents=True)
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "driver.cpp").write_text(PIPELINE_DRIVER)
    sources = [str(tmp_path / "driver.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"),
               str(COMPONENT / "panel_pipeline.cpp"), str(REPO / "ui_lvgl/src/heat_image.cpp"), *core]
    exe = tmp_path / "pipeline"
    cmd = [_cxx(), *FLAGS, "-O2", f"-I{tmp_path}", f"-I{COMPONENT}", f"-I{REPO / 'core/include'}",
           f"-I{REPO / 'ui_lvgl/include'}", *sources, "-o", str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr[-4000:]
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    assert run.returncode == 0, run.stdout[-3000:] + run.stderr[-2000:]
    print(run.stdout)
    result = {}
    for line in run.stdout.splitlines():
        head, *fields = line.split()
        result[head] = dict(f.split("=", 1) for f in fields)
    return result


def _panel_like_config() -> dict:
    """A fictional house shaped like the first panel install that boot-looped: 18 rooms (mostly
    rectangles, a few L-shapes), 4 of them outdoor, 22 pins, a 131 x 105 grid, power 3, and a
    920 x 736 map."""
    rooms = []
    for i in range(18):
        cx, cy = i % 6, i // 6
        x0, y0, x1, y1 = cx / 6, cy / 3, (cx + 1) / 6, (cy + 1) / 3
        if i % 5 == 2:  # an L-shape
            mx, my = (x0 + x1) / 2, (y0 + y1) / 2
            poly = [[x0, y0], [x1, y0], [x1, my], [mx, my], [mx, y1], [x0, y1]]
        else:
            poly = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
        rooms.append({"id": f"room_{i}", "name": f"Room {i}",
                      "polygon": [[round(x, 6), round(y, 6)] for x, y in poly],
                      "outdoor": i >= 14})
    pins = []
    for k in range(22):
        r = k % 18
        cx, cy = r % 6, r // 6
        pins.append({"id": f"pin_{k}", "kind": "temperature", "name": f"Sensor {k}",
                     "position": [round((cx + 0.25 + 0.5 * (k // 18)) / 6, 6), round((cy + 0.25) / 3, 6)],
                     "room": f"room_{r}", "entities": {"temperature": f"sensor.example_house_{k}"}})
    return {
        "version": 1,
        "canvas": {"width": 1280, "height": 800, "orientation": "landscape",
                   "map_area": {"x": 0, "y": 64, "width": 920, "height": 736}},
        "rooms": rooms, "pins": pins,
        "heat": {"grid_cols": 131, "grid_rows": 105, "idw_power": 3},
    }


CASES = {
    "64-combs": (lambda: _max_config(False), 1 + 256 // 4 + (256 + 800) // 4),
    "one-room-256-pins": (lambda: _max_config(True), 1 + 256 // 4 + (256 + 800) // 4),
    "panel-sized": (_panel_like_config, 1 + -(-105 // 4) + -(-(105 + 736) // 4)),
}


@pytest.mark.parametrize("case", list(CASES))
def test_every_pipeline_step_is_bounded_at_schema_maximums(gen, schema, tmp_path, case):
    make, expected_steps = CASES[case]
    r = _pipeline(gen, schema, make(), tmp_path)
    boot, frame = r["BOOT"], r["FRAME"]
    # Chunking really happens: 1 load step, then mask and rasterizer rows 4 at a time.
    assert int(boot["steps"]) == expected_steps, boot
    assert float(boot["worst_step_ms"]) < STEP_BUDGET_MS, boot
    assert float(frame["worst_step_ms"]) < STEP_BUDGET_MS, frame
    assert frame["redrawn"] == "1"


UNKNOWN_TEST = r"""
#include <cstdio>
#include "hearthmap/ui/format.h"
#include "hearthmap/view_model.h"
#include "hearthmap_gen/generated_config.h"
#include "readings_ingest.h"
using namespace hearthmap;
int main() {
  ConfigLoadResult r = hearthmap_gen::GeneratedConfigSource{}.load();
  if (!r.config) return 2;
  const Config& c = *r.config;
  Readings readings;
  // HA says "unknown" for the plant's moisture and never sends a unit for it.
  hearthmap_device::apply_state(readings, "sensor.example_monstera_moisture", "unknown", 5000);
  hearthmap_device::restamp(readings, true, 6000);  // R8 must leave a non-value alone
  UiState state = initial_ui_state(c);
  state.overlay_pin = std::string("plant_monstera");
  ViewModel vm;
  build_view_model(c, readings, 6000, state, vm);
  if (!vm.overlay) { std::printf("NO OVERLAY\n"); return 1; }
  for (const OverlayRow& row : vm.overlay->rows) {
    if (row.assessment.role != Role::Moisture) continue;
    std::printf("MOISTURE freshness=%d severity=%d has_value=%d has_raw=%d unit='%s' last=%lld verdict='%s'\n",
                int(row.assessment.freshness), int(row.assessment.severity),
                int(row.assessment.value.has_value()), int(row.assessment.raw.has_value()),
                row.unit.c_str(), row.last_updated ? (long long)*row.last_updated : -1LL,
                ui::thirst_verdict(row.assessment).c_str());
  }
  const PinView& pin = vm.pins[vm.overlay->pin_index];
  std::printf("PIN severity=%d hud_items=%zu\n", int(pin.primary.severity), vm.hud.items.size());
  return 0;
}
"""


def test_entity_that_stays_unknown_without_a_unit_renders_as_unknown(gen, schema, example, tmp_path):
    core = _core_sources()
    out = gen.emit_config_source(example, schema, "example.json")
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "t.cpp").write_text(UNKNOWN_TEST)
    text = _build_and_run(
        tmp_path,
        [str(tmp_path / "t.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"),
         str(COMPONENT / "readings_ingest.cpp"), str(REPO / "ui_lvgl/src/format.cpp"), *core],
        [tmp_path, COMPONENT, REPO / "core/include", REPO / "ui_lvgl/include"],
    )
    # Freshness::Unknown == 3, Severity::Stale == 3; no value, no raw, empty unit, time as given.
    assert "MOISTURE freshness=3 severity=3 has_value=0 has_raw=0 unit='' last=5000 verdict='no data'" in text, text
    assert "PIN severity=3 hud_items=0" in text, text
