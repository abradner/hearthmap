"""hm_bench: rendering benchmarks for the ESP32-P4 panel (tools/bench/README.md).

YAML:

    hm_bench:
      display_id: main_display     # the mipi_dsi display (frame-buffer measurements)
      # lvgl_id: ...               only with more than one lvgl instance
      # period: 30s                time between benchmark runs
      # start_delay: 8s            time after boot before the first run
      # lvgl_ppa_unit: false       force LVGL's PPA draw unit on under ESP-IDF 5.5 (experiment)

Not part of the hearthmap component: a benchmark harness only.
"""

from __future__ import annotations

from pathlib import Path

import esphome.codegen as cg
from esphome.components.lvgl import defines as lv_df
from esphome.components.lvgl.defines import CONF_LVGL_ID, add_lv_use
from esphome.components.lvgl.lvcode import LvglComponent
from esphome.components.mipi_dsi.display import MipiDsi
import esphome.config_validation as cv
from esphome.const import CONF_DISPLAY_ID, CONF_ID
from esphome.core import CORE
from esphome.helpers import write_file_if_changed

DEPENDENCIES = ["lvgl", "display"]

# The Blitter under test: ui_lvgl's interface and software reference, and the hearthmap
# component's PPA implementation, copied into the build so the bench times the shipped code.
REPO_ROOT = Path(__file__).resolve().parents[4]
BLITTER_FILES = {
    "hearthmap/ui/blitter.h": REPO_ROOT / "ui_lvgl/include/hearthmap/ui/blitter.h",
    "hm_blit/blitter.cpp": REPO_ROOT / "ui_lvgl/src/blitter.cpp",
    "hm_blit/ppa_blitter.h": REPO_ROOT / "components/hearthmap/ppa_blitter.h",
    "hm_blit/ppa_blitter.cpp": REPO_ROOT / "components/hearthmap/ppa_blitter.cpp",
}

CONF_PERIOD = "period"
CONF_START_DELAY = "start_delay"
CONF_LVGL_PPA_UNIT = "lvgl_ppa_unit"

hm_bench_ns = cg.esphome_ns.namespace("hm_bench")
HmBench = hm_bench_ns.class_("HmBench", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HmBench),
        cv.GenerateID(CONF_LVGL_ID): cv.use_id(LvglComponent),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(MipiDsi),
        cv.Optional(CONF_PERIOD, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_START_DELAY, default="8s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_LVGL_PPA_UNIT, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


_PPA_PATCHED = False


def _force_lvgl_ppa_unit() -> None:
    """ESPHome 2026.11.0's lvgl to_code writes LV_USE_PPA 0 below ESP-IDF 6.0, because LVGL
    9.6's PPA unit calls esp_cache_get_line_size_by_addr(), which ESP-IDF 5.5 does not have
    (components/lvgl/__init__.py, to_code). This experiment overrides that define and supplies
    the missing function as a macro in lv_conf.h: every LVGL draw buffer here is aligned to
    LV_DRAW_BUF_ALIGN (64), which equals the P4's L1 and (configured) L2 cache line, and
    lv_draw_ppa_private.h refuses to build unless CONFIG_LV_DRAW_BUF_ALIGN equals that line.
    The override wraps lvgl.defines.add_define, so it applies whenever lvgl's to_code runs."""
    global _PPA_PATCHED
    if _PPA_PATCHED:
        return
    _PPA_PATCHED = True
    original = lv_df.add_define

    def add_define(macro: str, value="1"):
        if macro == "LV_USE_PPA":
            value = "1"
        original(macro, value)

    lv_df.add_define = add_define
    original("LV_USE_PPA_IMG", "1")
    original("esp_cache_get_line_size_by_addr(addr)", "((void) (addr), (size_t) LV_DRAW_BUF_ALIGN)")


def _final_validate(config: dict) -> dict:
    # Before lvgl's to_code, which emits LV_USE_* from this set.
    add_lv_use("canvas", "image", "arc", "line", "label")
    if config[CONF_LVGL_PPA_UNIT]:
        _force_lvgl_ppa_unit()
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: dict) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_lvgl(await cg.get_variable(config[CONF_LVGL_ID])))
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    cg.add(var.set_period_ms(config[CONF_PERIOD].total_milliseconds))
    cg.add(var.set_start_delay_ms(config[CONF_START_DELAY].total_milliseconds))
    for rel, path in BLITTER_FILES.items():
        write_file_if_changed(Path(CORE.relative_src_path(rel)), path.read_text(encoding="utf-8"))
