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
