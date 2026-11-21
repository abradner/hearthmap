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
#include <clocale>
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
  // Locale independence: a comma-decimal locale must not change how "21.5" parses.
  const char* comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de_DE"};
  const char* used = nullptr;
  for (const char* l : comma_locales) {
    if (std::setlocale(LC_NUMERIC, l) != nullptr) { used = l; break; }
  }
  if (used != nullptr) {
    value("21.5", 21.5); value("-0.25e1", -2.5);
    std::setlocale(LC_NUMERIC, "C");
    std::printf("comma locale checked: %s\n", used);
  } else {
    std::printf("no comma locale installed\n");
  }
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


PERF_TEST = r"""
#include <cstdio>
#include <cstring>
#include "perf_stats.h"
using hearthmap_device::PerfStats;
using hearthmap_device::PerfWindow;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  PerfStats p(5000);
  CHECK(!p.due(1000));            // the first call starts the window
  CHECK(!p.due(5999));
  CHECK(p.due(6000));

  // Loop gaps, across a micros() wrap.
  p.loop_start(0xFFFFFF00u);
  p.loop_start(0x00000100u);      // 512 us later
  p.loop_start(0x00001100u);      // 4096 us later
  p.own_work(300, 1700);
  p.own_work(100, 0);

  // A refresh that flushed nothing does not count; one that flushed does.
  p.refresh_start(10000);
  p.refresh_ready(12000, 12);
  p.invalidated(100 * 50);
  p.refresh_start(20000);
  p.flush_start(21000);
  p.flush_finish(23000, 4000);
  p.flush_start(24000);
  p.flush_finish(25000, 1000);
  p.refresh_ready(70000, 70);
  // A REFR_READY without a start (registered mid-refresh) is ignored.
  p.refresh_ready(90000, 90);

  // Touch: read -> press latency, then click -> applied -> drawn.
  p.touch_read(100);              // a contact LVGL never saw pressed...
  p.touch_read(130);              // ...replaced by the next contact: timed from this one
  p.lvgl_pressed(180);
  p.lvgl_clicked(200);
  p.refresh_start(205000);        // a refresh before the click was applied: not its ack
  p.flush_start(205000); p.flush_finish(206000, 10);
  p.refresh_ready(207000, 207);
  p.events_applied(260, true);
  p.refresh_start(300000);
  p.flush_start(300000); p.flush_finish(301000, 10);
  p.refresh_ready(350000, 350);   // drawn 150 ms after the click

  PerfWindow w = p.take(6000);
  CHECK(w.window_ms == 5000);
  CHECK(w.loops == 3);
  CHECK(w.loop_gap_max == 4096);
  CHECK(w.own_view == 400 && w.own_heat == 1700 && w.own_max == 2000);
  CHECK(w.refreshes == 3);
  CHECK(w.refresh_max == 50000);
  CHECK(w.refresh_total == 50000 + 2000 + 50000);
  CHECK(w.flushes == 4 && w.flush_total == 2000 + 1000 + 1000 + 1000);
  CHECK(w.flushed_px == 5020);
  CHECK(w.invalidated_px == 5000);
  CHECK(w.touch_reads == 2 && w.presses == 1 && w.clicks == 1);
  CHECK(w.read_to_press_max_ms == 50);
  CHECK(w.acks == 1 && w.click_to_ack_max_ms == 150);

  // The next window starts empty; a click never applied is never acked; one drawn too late is
  // dropped rather than reported as a latency.
  CHECK(!p.due(10999));
  CHECK(p.due(11000));
  p.lvgl_clicked(7000);
  p.refresh_start(7100000); p.flush_start(7100000); p.flush_finish(7101000, 1);
  p.refresh_ready(7102000, 7102);
  p.lvgl_clicked(7500);           // a click that changed nothing on screen: not timed
  p.events_applied(7501, false);
  p.refresh_start(7600000); p.flush_start(7600000); p.flush_finish(7601000, 1);
  p.refresh_ready(7602000, 7602);
  p.lvgl_clicked(8000);
  p.events_applied(8001, true);
  p.refresh_start(11500000); p.flush_start(11500000); p.flush_finish(11501000, 1);
  p.refresh_ready(11502000, 11502);  // 3502 ms after the click: beyond kAckTimeoutMs
  w = p.take(11600);
  CHECK(w.loops == 0 && w.refreshes == 3);
  CHECK(w.acks == 0 && w.click_to_ack_max_ms == 0);

  // The longest refresh by cause: a tap's update, a heat repaint, else animation.
  p.take(12000);
  p.cause_tap();
  p.refresh_start(12000000); p.flush_start(12000000); p.flush_finish(12001000, 1);
  p.refresh_ready(12090000, 12090);  // 90 ms: a tap
  p.cause_heat();
  p.refresh_start(12100000); p.flush_start(12100000); p.flush_finish(12101000, 1);
  p.refresh_ready(12140000, 12140);  // 40 ms: heat
  p.refresh_start(12200000); p.flush_start(12200000); p.flush_finish(12201000, 1);
  p.refresh_ready(12210000, 12210);  // 10 ms: neither
  w = p.take(12300);
  CHECK(w.refresh_max_tap == 90000 && w.refresh_max_heat == 40000 && w.refresh_max_anim == 10000);

  // The log line: rates per second and the share of the window spent refreshing.
  PerfWindow f;
  f.window_ms = 5000;
  f.loops = 50;
  f.loop_gap_max = 412000;
  f.refreshes = 12;
  f.refresh_total = 2500000;  // 2.5 s of 5 s: 50 %
  f.refresh_max = 402000;
  f.flushed_px = 5000000;     // 1000 kpx/s
  f.sprite_frames = 1550;     // 310 a second
  f.sprite_divisor = 2;
  f.presented = 1500;
  f.present_fallbacks = 50;
  f.present_block = "covered";
  char line[640];
  const std::size_t n = PerfStats::format(f, line, sizeof(line));
  std::printf("%s\n", line);
  CHECK(n == std::strlen(line));
  CHECK(std::strstr(line, "loop 10/s, gap max 412 ms") != nullptr);
  CHECK(std::strstr(line, "12 refr, 2500 ms (50%), max 402 ms") != nullptr);
  CHECK(std::strstr(line, "1000 kpx/s flushed") != nullptr);
  CHECK(std::strstr(line, "sprites 310 frames/s (rate /2)") != nullptr);
  CHECK(std::strstr(line, "present 1500 / fallback 50 (covered)") != nullptr);
  char tiny[8];
  CHECK(PerfStats::format(f, tiny, sizeof(tiny)) == 7 && tiny[7] == '\0');
  std::printf("perf checks done, %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
"""


def test_perf_stats(tmp_path):
    (tmp_path / "perf_test.cpp").write_text(PERF_TEST)
    out = _build_and_run(
        tmp_path,
        [str(tmp_path / "perf_test.cpp"), str(COMPONENT / "perf_stats.cpp")],
        [COMPONENT],
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
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "hearthmap_gen/generated_config.h"
#include "panel_pipeline.h"
using namespace hearthmap;
using Stage = hearthmap_device::PanelPipeline::Stage;
// This thread's CPU time in ms: the driver is single-threaded and synchronous, so a step's CPU
// time is its work, and a runner pausing the process between instructions does not inflate it
// the way wall time does.
struct Clock {
  using time_point = double;
  static double now() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
  }
};
static double ms(double a, double b) { return b - a; }
int main() {
  hearthmap_gen::GeneratedConfigSource source;
  hearthmap_device::PanelPipeline::Builder builder;
  builder.steps = source.build_steps();
  builder.step = [&](Config& c, std::size_t k) { source.build_step(c, k); };
  // Boot three times and keep, per stage, the fastest run's total and worst step: timing on a
  // shared machine is noisy, and the work per step is the same every run.
  std::map<std::string, double> total, worst;
  std::map<std::string, int> steps;
  int all_steps = 0;
  std::unique_ptr<hearthmap_device::PanelPipeline> keep;
  for (int run = 0; run < 3; ++run) {
    auto p = std::make_unique<hearthmap_device::PanelPipeline>(builder, Palette{});
    std::map<std::string, double> t, w;
    std::map<std::string, int> n;
    int count = 0;
    while (p->stage() != Stage::Ready && p->stage() != Stage::Refused) {
      const std::string name = hearthmap_device::PanelPipeline::stage_name(p->stage());
      auto a = Clock::now();
      p->boot_step(4);  // the device's kBootRowsPerStep
      const double d = ms(a, Clock::now());
      t[name] += d;
      if (d > w[name]) w[name] = d;
      ++n[name];
      ++count;
    }
    if (p->stage() != Stage::Ready) {
      for (const auto& i : p->issues()) std::printf("ISSUE %s: %s\n", i.path.c_str(), i.message.c_str());
      std::printf("REFUSED\n");
      return 1;
    }
    for (const auto& kv : t) {
      if (run == 0 || kv.second < total[kv.first]) total[kv.first] = kv.second;
      if (run == 0 || w[kv.first] < worst[kv.first]) worst[kv.first] = w[kv.first];
    }
    steps = n;
    all_steps = count;
    keep = std::move(p);
  }
  hearthmap_device::PanelPipeline& p = *keep;
  std::printf("BOOT steps=%d\n", all_steps);
  for (const auto& kv : total) {
    std::printf("STAGE_%s steps=%d total_ms=%.4f worst_step_ms=%.4f\n", kv.first.c_str(), steps[kv.first],
                kv.second, worst[kv.first]);
  }
  std::vector<Rgb565> a(p.rasterizer().pixel_count()), b(p.rasterizer().pixel_count());
  p.set_heat_buffers(a.data(), b.data());
  Readings readings;
  for (std::size_t i = 0; i < hearthmap_gen::kSubscribedEntityCount; ++i) {
    Reading r; r.state = ReadingState::Value; r.value = 18.0 + double(i % 9); r.last_updated = 1000;
    readings.by_entity.emplace(hearthmap_gen::kSubscribedEntities[i].entity_id, r);
  }
  // Frame worst step: each frame does the same kinds of step, so keep the smallest of the three
  // frames' worst steps (a shared runner's noise hits one frame, not every one).
  double fworst = 1e9, ftotal = 0; int fsteps = 0, published = 0;
  bool front_untouched = true;
  p.set_heat_refresh_interval_ms(0);  // every frame's changed readings rebuild: worst case
  for (int frame = 0; frame < 3; ++frame) {
    const Rgb565* shown = p.front();
    const std::vector<Rgb565> before(shown, shown + p.rasterizer().pixel_count());
    double this_worst = 0;
    const std::uint32_t mono = 1000U * std::uint32_t(frame + 1);
    auto t0 = Clock::now();
    p.update(readings, 1000, initial_ui_state(p.config()), mono);
    double d = ms(t0, Clock::now()); if (d > this_worst) this_worst = d; ++fsteps;
    ftotal += d;
    if (p.front() != shown) front_untouched = false;
    bool done = false;
    while (!done) {
      auto t = Clock::now();
      done = p.heat_step(1, mono);  // the device's kFieldRowsPerStep
      double e = ms(t, Clock::now()); if (e > this_worst) this_worst = e; ++fsteps;
      ftotal += e;  // only the pipeline calls are timed
      // The image on screen must not change until the rebuild publishes (Codex P1, D-038): the
      // front pointer stays put and its pixels unchanged. Checked outside the timed intervals.
      if (!done && (p.front() != shown || !std::equal(before.begin(), before.end(), shown))) {
        front_untouched = false;
      }
    }
    if (this_worst < fworst) fworst = this_worst;
    published += p.take_published() ? 1 : 0;
    readings.by_entity.begin()->second.value += 1.0;  // force a field change, so a redraw
  }
  std::printf("FRAME steps=%d total_ms=%.2f worst_step_ms=%.3f published=%d front_untouched=%d\n", fsteps,
              ftotal, fworst, published, int(front_untouched));
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


def _boot_steps(doc: dict, rows_per_step: int = 4) -> int:
    """Steps the pipeline must take: build 2 + rooms + pins, validate 3 + rooms + pins, prepare 5
    (four colour tables and the scanner), then mask and rasterizer rows `rows_per_step` at a
    time."""
    grid_rows = doc["heat"]["grid_rows"]
    height = doc["canvas"]["map_area"]["height"]
    ceil = lambda n: -(-n // rows_per_step)  # noqa: E731
    units = len(doc["rooms"]) + len(doc["pins"])
    return (2 + units) + (3 + units) + 5 + ceil(grid_rows) + ceil(grid_rows + height)


CASES = {
    "64-combs": lambda: _max_config(False),
    "one-room-256-pins": lambda: _max_config(True),
    "panel-sized": _panel_like_config,
}

# Device calibration, from the second hardware run (ESP32-P4 at 360 MHz, a config shaped like
# the "panel-sized" case): its mask stage took 50 ms there against 0.24 ms here, its raster
# stage 229 ms against 1.25 ms, a redraw frame about 825 ms against about 15 ms. So boot work
# (double-heavy) runs about 200x slower on the panel and frame work (float and integer) about
# 55x. A step should stay under ~50 ms there (ESPHome warns past 50 ms per loop call). The
# bounds use the mean step per stage, robust to one cold-cache outlier, with headroom (300x and
# 100x), plus a cap on any single step, loose enough for a shared CI runner, that catches gross
# regressions (the pre-fix per-pixel construction took 37 ms per step). Whole-config units
# (the pre-fix load step) are caught by the step count, which assumes one room or pin per step.
BOOT_SLOWDOWN = 300
FRAME_SLOWDOWN = 100
DEVICE_STEP_MS = 50.0
HOST_STEP_CAP_MS = 2.0


@pytest.mark.parametrize("case", list(CASES))
def test_every_pipeline_step_is_bounded_at_schema_maximums(gen, schema, tmp_path, case):
    doc = CASES[case]()
    r = _pipeline(gen, schema, doc, tmp_path)
    boot, frame = r["BOOT"], r["FRAME"]
    assert int(boot["steps"]) == _boot_steps(doc), boot  # the work really is chunked
    stages = {k: v for k, v in r.items() if k.startswith("STAGE_")}
    assert set(stages) == {f"STAGE_{n}" for n in ("build", "validate", "prepare", "mask", "raster")}
    for key, stage in stages.items():
        mean = float(stage["total_ms"]) / int(stage["steps"])
        assert mean * BOOT_SLOWDOWN < DEVICE_STEP_MS, (key, stage)
        assert float(stage["worst_step_ms"]) < HOST_STEP_CAP_MS, (key, stage)
    frame_mean = float(frame["total_ms"]) / int(frame["steps"])
    assert frame_mean * FRAME_SLOWDOWN < DEVICE_STEP_MS, frame
    assert float(frame["worst_step_ms"]) < HOST_STEP_CAP_MS, frame
    assert frame["published"] == "3", frame
    assert frame["front_untouched"] == "1", frame  # the shown image never half-written


# ---------------------------------------------------------------------------------------------
# When the heat image is rebuilt, and that taps never wait for it (docs/decisions.md D-047).
#
# On the panel one rebuild takes about a second. Rebuilding for every changed reading kept the
# pipeline permanently mid-rebuild with chatty sensors, and taps, applied only between frames,
# waited behind it. The view model is now rebuilt at once on every update(); the heat image is
# rebuilt at once for the first draw and a layer change, at most once per interval for changed
# readings, and never for unchanged ones.
# ---------------------------------------------------------------------------------------------

HEAT_POLICY_TEST = r"""
#include <cstdio>
#include <vector>
#include "hearthmap_gen/generated_config.h"
#include "panel_pipeline.h"
using namespace hearthmap;
using Pipeline = hearthmap_device::PanelPipeline;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// Runs heat work to idle at `mono`; returns how many swaps happened.
static int drain(Pipeline& p, std::uint32_t mono) {
  int guard = 0;
  while (!p.heat_step(1, mono) && ++guard < 1000000) {}
  return p.take_published() ? 1 : 0;
}

int main() {
  hearthmap_gen::GeneratedConfigSource source;
  Pipeline::Builder builder;
  builder.steps = source.build_steps();
  builder.step = [&](Config& c, std::size_t k) { source.build_step(c, k); };
  Pipeline p(builder, Palette{});
  while (p.stage() != Pipeline::Stage::Ready && p.stage() != Pipeline::Stage::Refused) p.boot_step(4);
  if (p.stage() != Pipeline::Stage::Ready) { std::printf("REFUSED\n"); return 2; }
  std::vector<Rgb565> a(p.rasterizer().pixel_count()), b(p.rasterizer().pixel_count());
  p.set_heat_buffers(a.data(), b.data());
  p.set_heat_refresh_interval_ms(15000);

  Readings rs;
  for (std::size_t i = 0; i < hearthmap_gen::kSubscribedEntityCount; ++i) {
    Reading r; r.state = ReadingState::Value; r.value = 20.0 + double(i % 5) * 0.4; r.last_updated = 1000;
    rs.by_entity.emplace(hearthmap_gen::kSubscribedEntities[i].entity_id, r);
  }
  // A temperature entity: changing it changes the temperature layer's samples.
  Reading* temp = nullptr;
  for (const Pin& pin : p.config().pins) {
    for (const EntityBinding& bnd : pin.entities) {
      if (bnd.role == Role::Temperature && feeds_layer(pin.kind, Role::Temperature) && temp == nullptr) {
        temp = &rs.by_entity.find(bnd.entity_id)->second;
      }
    }
  }
  if (temp == nullptr) { std::printf("NO TEMPERATURE PIN\n"); return 2; }
  UiState st = initial_ui_state(p.config());
  st.active_layer = Layer::Temperature;

  // The first image is drawn at once, whatever the interval; the map is dimmed until it lands.
  p.update(rs, 1000, st, 0);
  CHECK(p.heat_behind());
  CHECK(drain(p, 0) == 1);
  CHECK(!p.heat_behind());

  // Unchanged readings never rebuild, even long after the interval.
  p.update(rs, 1000, st, 20000);
  CHECK(drain(p, 20000) == 0);
  p.update(rs, 1000, st, 40000);
  CHECK(drain(p, 40000) == 0);
  CHECK(p.heat_rebuilds() == 1);

  // A changed reading after the interval rebuilds at once (the last start was at 0).
  temp->value += 1.0;
  p.update(rs, 1000, st, 40000);
  CHECK(drain(p, 40000) == 1);
  // Readings changing every frame within the interval: no further rebuild, though the view
  // model (pins, HUD, overlay) follows every one of them.
  int extra = 0;
  for (std::uint32_t t = 40500; t < 55000; t += 500) {
    temp->value += 0.25;
    p.update(rs, 1000, st, t);
    extra += drain(p, t);
  }
  CHECK(extra == 0);
  CHECK(p.heat_rebuilds() == 2);
  // The held-back change lands once the interval has passed, with no new reading (heat_step
  // alone), and exactly once.
  CHECK(drain(p, 55000) == 1);
  CHECK(drain(p, 56000) == 0);
  CHECK(p.heat_rebuilds() == 3);

  // Only the layer changed, well inside the interval: rebuilt at once, dimmed until it lands.
  std::vector<Layer> others;
  for (Layer l : p.view().tabs) if (l != Layer::Temperature) others.push_back(l);
  if (others.size() < 2) { std::printf("NEED THREE LAYERS\n"); return 2; }
  UiState st2 = st;
  st2.active_layer = others[0];
  p.update(rs, 1000, st2, 56500);
  CHECK(p.view().active_layer == others[0]);
  CHECK(p.heat_behind());
  CHECK(drain(p, 56500) == 1);
  CHECK(!p.heat_behind());

  // Back to a layer whose image is still kept (temperature, in the other buffer): shown at once
  // as a layer switch, with no rebuild (D-055).
  {
    const std::uint32_t n = p.heat_rebuilds();
    UiState back = st2;
    back.active_layer = Layer::Temperature;
    p.update(rs, 1000, back, 56900);
    CHECK(!p.heat_behind());
    CHECK(p.take_published());
    CHECK(p.published_switch());
    CHECK(drain(p, 56900) == 0);
    CHECK(p.heat_rebuilds() == n);
  }

  // A tap during a rebuild is in the very next view model, without waiting for the image.
  UiState st3 = st2;
  st3.active_layer = others[1];  // never drawn
  p.update(rs, 1000, st3, 57000);
  for (int k = 0; k < 3; ++k) CHECK(!p.heat_step(1, 57000));  // rebuild in flight
  UiState st4 = apply(p.config(), st3, UiEvent::tap_hud_item(p.config().pins[0].id));
  CHECK(st4.overlay_pin.has_value());
  p.update(rs, 1000, st4, 57010);
  CHECK(p.view().overlay.has_value());
  CHECK(p.view().active_layer == others[1]);
  CHECK(p.heat_behind());               // still another layer's image: veiled
  CHECK(!p.heat_step(1, 57010));        // the rebuild is still going
  CHECK(!p.take_published());

  // A layer change during a rebuild restarts it for the new layer (others[0], whose buffer the
  // rebuild in flight took), and that is what lands.
  UiState st5 = st4;
  st5.active_layer = others[0];
  p.update(rs, 1000, st5, 57020);
  CHECK(p.heat_behind());
  CHECK(drain(p, 57020) == 1);
  CHECK(!p.published_switch());
  CHECK(!p.heat_behind());
  CHECK(p.view().active_layer == others[0]);

  // Tabbing away and straight back cancels the rebuild: the shown image is current again.
  const std::uint32_t before = p.heat_rebuilds();
  UiState st6 = st5;
  st6.active_layer = others[1];
  p.update(rs, 1000, st6, 57100);
  CHECK(!p.heat_step(1, 57100));
  p.update(rs, 1000, st5, 57110);
  CHECK(!p.heat_behind());
  CHECK(drain(p, 57110) == 0);
  CHECK(p.heat_rebuilds() == before + 1);

  std::printf("heat policy checks done, %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
"""


def test_heat_rebuild_policy_and_immediate_taps(gen, schema, example, tmp_path):
    core = _core_sources()
    out = gen.emit_config_source(example, schema, "example.json")
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "t.cpp").write_text(HEAT_POLICY_TEST)
    text = _build_and_run(
        tmp_path,
        [str(tmp_path / "t.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"),
         str(COMPONENT / "panel_pipeline.cpp"), str(REPO / "ui_lvgl/src/heat_image.cpp"), *core],
        [tmp_path, COMPONENT, REPO / "core/include", REPO / "ui_lvgl/include"],
    )
    assert "0 failures" in text, text


POOL_TEST = r"""
#include <cstdio>
#include <vector>
#include "hearthmap_gen/generated_config.h"
#include "panel_pipeline.h"
using namespace hearthmap;
using Pipeline = hearthmap_device::PanelPipeline;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static int drain(Pipeline& p, std::uint32_t mono) {
  int guard = 0;
  while (!p.heat_step(1, mono) && ++guard < 1000000) {}
  return p.take_published() ? 1 : 0;
}

int main() {
  hearthmap_gen::GeneratedConfigSource source;
  Pipeline::Builder builder;
  builder.steps = source.build_steps();
  builder.step = [&](Config& c, std::size_t k) { source.build_step(c, k); };
  Pipeline p(builder, Palette{});
  while (p.stage() != Pipeline::Stage::Ready && p.stage() != Pipeline::Stage::Refused) p.boot_step(4);
  if (p.stage() != Pipeline::Stage::Ready) return 2;
  const std::size_t n = p.rasterizer().pixel_count();
  std::vector<std::vector<Rgb565>> bufs(5, std::vector<Rgb565>(n));
  std::vector<Rgb565*> ptrs;
  for (auto& b : bufs) ptrs.push_back(b.data());
  p.set_heat_buffers(ptrs);
  p.set_heat_refresh_interval_ms(1000);
  // The row hook: everything after `bake_from` is baked.
  std::uint32_t bake_from = 1u << 30;
  std::uint32_t hook_calls = 0;
  std::uint32_t now_ms = 0;
  p.set_row_hook([&](Rgb565*, std::int32_t, std::int32_t, std::int32_t) {
    ++hook_calls;
    return now_ms >= bake_from;
  });

  Readings rs;
  for (std::size_t i = 0; i < hearthmap_gen::kSubscribedEntityCount; ++i) {
    Reading r; r.state = ReadingState::Value; r.value = 20.0 + double(i % 5) * 0.4; r.last_updated = 1000;
    rs.by_entity.emplace(hearthmap_gen::kSubscribedEntities[i].entity_id, r);
  }
  UiState st = initial_ui_state(p.config());
  st.active_layer = Layer::Temperature;
  const std::size_t layers = p.config().layers.size();
  p.update(rs, 1000, st, now_ms);
  const std::size_t tabs = p.view().tabs.size();
  if (tabs + 1 > 5) { std::printf("TOO MANY LAYERS\n"); return 2; }

  // The shown layer first, then every other layer in the background, one per interval.
  CHECK(drain(p, now_ms) == 1);
  CHECK(!p.front_baked());
  CHECK(hook_calls > 0);
  for (int k = 0; k < 10; ++k) {
    now_ms += 1000;
    p.update(rs, 1000, st, now_ms);
    CHECK(drain(p, now_ms) == 0);  // background images are not shown
  }
  CHECK(p.cached_layers() == tabs);
  const std::uint32_t built = p.heat_rebuilds();
  CHECK(built == tabs);

  // Every other tab is shown at once, as a layer switch, with no rebuild.
  for (Layer l : p.view().tabs) {
    UiState s2 = st;
    s2.active_layer = l;
    p.update(rs, 1000, s2, now_ms);
    CHECK(!p.heat_behind());
    if (l != Layer::Temperature) {
      CHECK(p.take_published());
      CHECK(p.published_switch());
    }
    CHECK(drain(p, now_ms) == 0);
  }
  CHECK(p.heat_rebuilds() == built);
  p.update(rs, 1000, st, now_ms);
  (void)p.take_published();

  // The walls become available: every image is redrawn, the shown one at once.
  bake_from = now_ms;
  p.request_rebuild_all();
  CHECK(drain(p, now_ms) == 1);
  CHECK(p.front_baked());
  CHECK(!p.published_switch());
  for (int k = 0; k < 10; ++k) {
    now_ms += 1000;
    p.update(rs, 1000, st, now_ms);
    drain(p, now_ms);
  }
  CHECK(p.cached_layers() == tabs);
  for (Layer l : p.view().tabs) {
    UiState s2 = st;
    s2.active_layer = l;
    p.update(rs, 1000, s2, now_ms);
    CHECK(p.front_baked());  // the background copies were redrawn with the walls too
    drain(p, now_ms);
  }
  (void)layers;
  std::printf("pool checks done, %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
"""


def test_layer_images_are_kept_and_shown_at_once(gen, schema, example, tmp_path):
    core = _core_sources()
    out = gen.emit_config_source(example, schema, "example.json")
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "t.cpp").write_text(POOL_TEST)
    text = _build_and_run(
        tmp_path,
        [str(tmp_path / "t.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"),
         str(COMPONENT / "panel_pipeline.cpp"), str(REPO / "ui_lvgl/src/heat_image.cpp"), *core],
        [tmp_path, COMPONENT, REPO / "core/include", REPO / "ui_lvgl/include"],
    )
    assert "0 failures" in text, text


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


# A stand-in for lvgl.h: just the types GeneratedPhotos and ports.h touch.
LVGL_STUB = r"""
#pragma once
#include <cstdint>
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 0x12
typedef struct {
  uint32_t magic : 8; uint32_t cf : 8; uint32_t flags : 16;
  uint32_t w : 16; uint32_t h : 16; uint32_t stride : 16; uint32_t reserved_2 : 16;
} lv_image_header_t;
typedef struct { lv_image_header_t header; uint32_t data_size; const uint8_t* data; } lv_image_dsc_t;
"""

PHOTOS_TEST = r"""
#include <cstdio>
#include "generated_photos.h"
int main() {
  hearthmap_device::GeneratedPhotos photos;
  int fail = 0;
  if (photos.plant_photo("fern") != nullptr || photos.plant_photo_sized("fern", 8) != nullptr) {
    std::printf("FAIL before init\n"); ++fail;
  }
  photos.init();
  const lv_image_dsc_t* fern = photos.plant_photo("fern");
  const lv_image_dsc_t* moss = photos.plant_photo("moss");
  const lv_image_dsc_t* fern4 = photos.plant_photo_sized("fern", 4);
  if (!fern || !moss || fern->header.w != 8 || fern->header.stride != 16 || fern->data_size != 128 ||
      fern->header.magic != LV_IMAGE_HEADER_MAGIC || fern->header.cf != LV_COLOR_FORMAT_RGB565) {
    std::printf("FAIL largest descriptor\n"); ++fail;
  }
  if (photos.plant_photo_sized("fern", 8) != fern || !fern4 || fern4->header.w != 4 ||
      fern4->header.h != 4 || fern4->data_size != 32 || photos.plant_photo_sized("fern", 5) != nullptr) {
    std::printf("FAIL sized descriptors\n"); ++fail;
  }
  photos.init();  // must be a no-op: the Screen already holds these pointers
  if (photos.plant_photo("fern") != fern || photos.plant_photo("moss") != moss ||
      photos.plant_photo_sized("fern", 4) != fern4) {
    std::printf("FAIL pointers moved\n"); ++fail;
  }
  if (photos.plant_photo("cactus") != nullptr) { std::printf("FAIL unknown id\n"); ++fail; }
  std::printf("photos checks done, %d failures\n", fail);
  return fail;
}
"""


def test_generated_photos_init_once_keeps_descriptors_stable(gen, tmp_path):
    from PIL import Image

    files = {}
    for pid, colour in (("fern", (0, 255, 0)), ("moss", (0, 128, 0))):
        f = tmp_path / f"{pid}.png"
        Image.new("RGB", (8, 8), colour).save(f)
        files[pid] = f
    header, source = gen.emit_photos(files, sizes=(8, 4))
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/photos.h").write_text(header)
    (tmp_path / "hearthmap_gen/photos.cpp").write_text(source)
    (tmp_path / "lvgl.h").write_text(LVGL_STUB)
    (tmp_path / "t.cpp").write_text(PHOTOS_TEST)
    out = _build_and_run(
        tmp_path,
        [str(tmp_path / "t.cpp"), str(tmp_path / "hearthmap_gen/photos.cpp"),
         str(COMPONENT / "generated_photos.cpp")],
        [tmp_path, COMPONENT, REPO / "core/include", REPO / "ui_lvgl/include"],
    )
    assert "0 failures" in out, out


NAME_TEST = r"""
#include <cstdio>
#include "hearthmap_gen/generated_config.h"
using namespace hearthmap;
int main() {
  ConfigLoadResult r = hearthmap_gen::GeneratedConfigSource{}.load();
  if (!r.config) { for (auto& i : r.issues) std::printf("ISSUE %s %s\n", i.path.c_str(), i.message.c_str()); return 1; }
  const std::string& n = r.config->rooms[0].name;
  std::printf("NAME size=%zu first=%d rest=%s\n", n.size(), int(n[0]), n.c_str() + 1);
  return 0;
}
"""


def test_names_with_an_embedded_nul_survive_generation(gen, schema, example, tmp_path):
    # Schema-valid, kept by the host's JSON source: the device must hold the same string.
    core = _core_sources()
    example["rooms"][0]["name"] = "\x00Room"
    assert gen.check_schema(schema, example) == []
    out = gen.emit_config_source(example, schema, "e.json")
    (tmp_path / "hearthmap_gen").mkdir()
    (tmp_path / "hearthmap_gen/generated_config.h").write_text(out.header)
    (tmp_path / "hearthmap_gen/generated_config.cpp").write_text(out.source)
    (tmp_path / "t.cpp").write_text(NAME_TEST)
    text = _build_and_run(
        tmp_path,
        [str(tmp_path / "t.cpp"), str(tmp_path / "hearthmap_gen/generated_config.cpp"), *core],
        [tmp_path, REPO / "core/include"],
    )
    assert "NAME size=5 first=0 rest=Room" in text, text


BLITTER_SELECT_TEST = r"""
#include <cstdio>
#include <cstring>
#include "blitter_select.h"
using namespace esphome::hearthmap_panel;
int main() {
  int failures = 0;
  const char* name = nullptr;
  auto sw = make_blitter(BlitterKind::Software, &name);
  if (!sw || std::strcmp(name, "software") != 0) { std::printf("FAIL software: %s\n", name); ++failures; }
  auto ppa = make_blitter(BlitterKind::Ppa, &name);
  // Off the P4 the PPA request must still give a working blitter, and say it fell back.
  if (!ppa || std::strstr(name, "software") == nullptr) { std::printf("FAIL ppa fallback: %s\n", name); ++failures; }
  std::uint16_t px[4] = {0, 0, 0, 0};
  ppa->fill(hearthmap::ui::Surface565{px, 2, 2, 2}, 0, 0, 2, 2, 0x1234);
  if (px[3] != 0x1234) { std::printf("FAIL fallback does not draw\n"); ++failures; }
  std::printf("%d failures\n", failures);
  return failures;
}
"""


def test_blitter_select_falls_back_to_software_off_the_p4(tmp_path):
    (tmp_path / "blitter_select_test.cpp").write_text(BLITTER_SELECT_TEST)
    out = _build_and_run(
        tmp_path,
        [str(tmp_path / "blitter_select_test.cpp"), str(COMPONENT / "blitter_select.cpp"),
         str(REPO / "ui_lvgl" / "src" / "blitter.cpp")],
        [COMPONENT, REPO / "ui_lvgl" / "include"],
    )
    assert "0 failures" in out


PANEL_ROTATION_TEST = r"""
#include <cstdio>
#include <cstdint>
#include <vector>
#include "panel_rotation.h"
using namespace esphome::hearthmap_panel;

// The lvgl component's software rotation (ESPHome 2026.11.0, lvgl_esphome.cpp, draw_buffer_),
// copied as written, with draw_rounding 1. `width_`/`height_` are the native panel size.
static NativeBlock esphome_way(std::vector<uint16_t>& dst, const uint16_t* ptr, int x1, int y1, int width,
                               int height, int rotation, int width_, int height_) {
  const int height_rounded = height;
  const int ax1 = x1, ay1 = y1;
  if (rotation == 90) {
    for (int x = height; x-- != 0;)
      for (int y = 0; y != width; y++) dst[y * height_rounded + x] = *ptr++;
    y1 = ax1;
    x1 = width_ - ay1 - height;
    height = width;
    width = height_rounded;
  } else {  // 270
    for (int x = 0; x != height; x++)
      for (int y = width; y-- != 0;) dst[y * height_rounded + x] = *ptr++;
    x1 = ay1;
    y1 = height_ - ax1 - width;
    height = width;
    width = height_rounded;
  }
  return {x1, y1, width, height};
}

int main() {
  int failures = 0;
  const int nw = 800, nh = 1280, w = 13, h = 7, x = 101, y = 57;
  std::vector<uint16_t> src(w * h);
  for (int i = 0; i < w * h; ++i) src[i] = static_cast<uint16_t>(i * 37 + 5);
  for (int rot : {90, 270}) {
    std::vector<uint16_t> a(w * h), b(w * h);
    NativeBlock ea = esphome_way(a, src.data(), x, y, w, h, rot, nw, nh);
    NativeBlock eb = to_native(b.data(), src.data(), w, x, y, w, h, rot == 90 ? 1 : 3, nw, nh);
    if (ea.x != eb.x || ea.y != eb.y || ea.w != eb.w || ea.h != eb.h) {
      std::printf("FAIL rot %d block %d,%d %dx%d vs %d,%d %dx%d\n", rot, ea.x, ea.y, ea.w, ea.h, eb.x, eb.y, eb.w, eb.h);
      ++failures;
    }
    if (a != b) { std::printf("FAIL rot %d pixels differ\n", rot); ++failures; }
  }
  // A source stride wider than the tile is honoured.
  std::vector<uint16_t> wide(20 * h, 0xFFFF), out(w * h), ref(w * h);
  for (int r = 0; r < h; ++r)
    for (int c = 0; c < w; ++c) wide[r * 20 + c] = src[r * w + c];
  to_native(out.data(), wide.data(), 20, x, y, w, h, 1, nw, nh);
  to_native(ref.data(), src.data(), w, x, y, w, h, 1, nw, nh);
  if (out != ref) { std::printf("FAIL stride\n"); ++failures; }
  if (to_native(out.data(), src.data(), w, x, y, w, h, 2, nw, nh).w != 0) { std::printf("FAIL 180\n"); ++failures; }
  std::printf("%d failures\n", failures);
  return failures;
}
"""


def test_panel_rotation_matches_the_lvgl_component(tmp_path):
    (tmp_path / "rot_test.cpp").write_text(PANEL_ROTATION_TEST)
    out = _build_and_run(tmp_path, [str(tmp_path / "rot_test.cpp")], [COMPONENT])
    assert "0 failures" in out
