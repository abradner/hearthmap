"""hearthmap: a house heatmap wall panel on LVGL, fed by Home Assistant.

YAML:

    hearthmap:
      config: hearthmap.json   # a schema-valid config (schema/hearthmap.schema.json)
      photos: photos           # directory holding <photo id>.png for plant pins
      time_id: ha_time         # an ESPHome time source; nothing is judged before it is valid
      # optional: lvgl_id, refresh_interval (30s), min_redraw_interval (500ms),
      #           heat_refresh_interval (15s)

Build-time work lives in hearthmap_codegen.py (pure Python, unit-tested); this file is only the
ESPHome surface. See README.md for how core and ui_lvgl reach the build.
"""

from __future__ import annotations

import json
from pathlib import Path

import esphome.codegen as cg
from esphome.components import time as time_
from esphome.components.lvgl.defines import CONF_LVGL_ID, add_lv_use, get_lv_fonts_used
from esphome.components.lvgl.lvcode import LvglComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority

from . import hearthmap_codegen as gen

CODEOWNERS = ["@abradner"]
DEPENDENCIES = ["api", "lvgl", "time"]

CONF_CONFIG = "config"
CONF_PHOTOS = "photos"
CONF_REFRESH_INTERVAL = "refresh_interval"
CONF_MIN_REDRAW_INTERVAL = "min_redraw_interval"
CONF_HEAT_REFRESH_INTERVAL = "heat_refresh_interval"

# The component lives at <repo>/components/hearthmap; core/, ui_lvgl/ and schema/ are siblings
# of components/ in the same checkout. ESPHome clones the whole repo (depth 1) for a github://
# source, and a local source is a working tree, so they are present either way.
REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "schema" / "hearthmap.schema.json"

hearthmap_panel_ns = cg.esphome_ns.namespace("hearthmap_panel")
HearthmapComponent = hearthmap_panel_ns.class_("HearthmapComponent", cg.Component)


def _load_schema() -> dict:
    if not SCHEMA_PATH.is_file():
        raise cv.Invalid(
            f"hearthmap: {SCHEMA_PATH} not found. The component must be used from a checkout of "
            "the hearthmap repository (external_components: github://... or a local path to its "
            "components/ directory), not copied out on its own."
        )
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def _load_document(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise cv.Invalid(f"hearthmap config {path.name}: not valid JSON: {e}") from e


def _validate_document(config: dict) -> dict:
    """Schema check and photo check at config time, so `esphome config` already fails on a bad
    config. The semantic rules are core's validate(), run at boot (README.md)."""
    path = Path(config[CONF_CONFIG])
    doc = _load_document(path)
    errors = gen.check_schema(_load_schema(), doc)
    if errors:
        listed = "\n".join(f"  {p or '/'}: {m}" for p, m in errors[:20])
        more = f"\n  ...and {len(errors) - 20} more" if len(errors) > 20 else ""
        raise cv.Invalid(f"hearthmap config {path.name} does not match the schema:\n{listed}{more}", [CONF_CONFIG])
    try:
        gen.check_photos(doc, Path(config[CONF_PHOTOS]) if CONF_PHOTOS in config else None)
    except gen.ConfigError as e:
        raise cv.Invalid(f"hearthmap config {path.name}: {e}", [CONF_PHOTOS]) from e
    try:
        # Anything C++ cannot hold (an integer outside its field's type) fails here, at config
        # time, rather than in to_code.
        gen.emit_config_source(doc, _load_schema(), path.name)
    except gen.ConfigError as e:
        raise cv.Invalid(f"hearthmap config {path.name}: {e}", [CONF_CONFIG]) from e
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HearthmapComponent),
            cv.GenerateID(CONF_LVGL_ID): cv.use_id(LvglComponent),
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Required(CONF_CONFIG): cv.file_,
            cv.Optional(CONF_PHOTOS): cv.directory,
            cv.Optional(CONF_REFRESH_INTERVAL, default="30s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(minutes=10)),
            ),
            cv.Optional(CONF_MIN_REDRAW_INTERVAL, default="500ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=50), max=cv.TimePeriod(seconds=10)),
            ),
            # How often changed readings may rebuild the heat image (about a second of work on the
            # panel). The first image and a layer change are never held back (README.md, "Cost").
            cv.Optional(CONF_HEAT_REFRESH_INTERVAL, default="15s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(minutes=10)),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_document,
)


# LVGL widgets (and the flex layout) ui_lvgl uses from C++, and the built-in fonts its builtin_fonts() names
# (ui_lvgl/include/hearthmap/ui/fonts.h). LVGL compiles only what a build declares.
UI_WIDGETS = ("label", "line", "image", "arc", "flex")
UI_FONTS = tuple(f"montserrat_{px}" for px in (20, 22, 24, 26, 28, 30, 34))


def _final_validate(config: dict) -> dict:
    # Done at validation time so it precedes lvgl's to_code, which emits the LV_USE_* and
    # LV_FONT_* defines from these sets.
    add_lv_use(*UI_WIDGETS)
    get_lv_fonts_used().update(UI_FONTS)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _write_build_tree(config: dict) -> None:
    """Generate the tables and photos and copy core/ui_lvgl into the firmware's src/ tree."""
    doc_path = Path(config[CONF_CONFIG])
    doc = _load_document(doc_path)
    generated = gen.emit_config_source(doc, _load_schema(), doc_path.name)
    photos = gen.check_photos(doc, Path(config[CONF_PHOTOS]) if CONF_PHOTOS in config else None)
    photos_h, photos_cpp = gen.emit_photos(photos)

    lib = gen.library_files(REPO_ROOT)
    files: dict[str, bytes] = {rel: src.read_bytes() for rel, src in lib.items()}
    files[f"{gen.GEN_DIR}/generated_config.h"] = generated.header.encode()
    files[f"{gen.GEN_DIR}/generated_config.cpp"] = generated.source.encode()
    files[f"{gen.GEN_DIR}/photos.h"] = photos_h.encode()
    files[f"{gen.GEN_DIR}/photos.cpp"] = photos_cpp.encode()
    owned = {gen.LIB_DIR, gen.GEN_DIR} | gen.public_include_roots(lib)
    gen.sync_tree(Path(CORE.relative_src_path()), files, owned)


@coroutine_with_priority(CoroPriority.FINAL)
async def _psram_malloc() -> None:
    """Let plain malloc/new use PSRAM for large blocks. The psram component sets
    CONFIG_SPIRAM_USE_CAPS_ALLOC (PSRAM only through heap_caps_malloc), but ui_lvgl's
    HeatRasterizer keeps a per-pixel room index in a std::vector (about 620 KiB for the example),
    more than the P4's internal RAM can spare. With CONFIG_SPIRAM_USE_MALLOC, blocks above
    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KiB by default) go to PSRAM and small ones stay
    internal. Run at FINAL priority so it lands after the psram component's own setting."""
    from esphome.components.esp32 import add_idf_sdkconfig_option

    add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_CAPS_ALLOC", False)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_MALLOC", True)


async def to_code(config: dict) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_lvgl(await cg.get_variable(config[CONF_LVGL_ID])))
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_refresh_interval_ms(config[CONF_REFRESH_INTERVAL].total_milliseconds))
    cg.add(var.set_min_redraw_interval_ms(config[CONF_MIN_REDRAW_INTERVAL].total_milliseconds))
    cg.add(var.set_heat_refresh_interval_ms(config[CONF_HEAT_REFRESH_INTERVAL].total_milliseconds))
    # Subscribing to HA states from C++ needs the API's state-subscription code compiled in.
    cg.add_define("USE_API_HOMEASSISTANT_STATES")

    _write_build_tree(config)
    if CORE.is_esp32 and "psram" in CORE.config:
        CORE.add_job(_psram_malloc)

    if CORE.using_toolchain_esp_idf:
        from esphome.components.esp32 import add_src_cmake

        # Core is built with -ffp-contract=off on the host so float results match bit for bit;
        # give its copy in the firmware the same flag.
        add_src_cmake(
            'file(GLOB_RECURSE hearthmap_lib_sources "${CMAKE_CURRENT_SOURCE_DIR}/'
            + gen.LIB_DIR
            + '/*.cpp")\n'
            'set_source_files_properties(${hearthmap_lib_sources} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")'
        )
