"""hearthmap: a house heatmap wall panel on LVGL, fed by Home Assistant.

YAML:

    hearthmap:
      config: hearthmap.json   # a schema-valid config (schema/hearthmap.schema.json)
      photos: photos           # directory holding <photo id>.png for plant pins
      time_id: ha_time         # an ESPHome time source; nothing is judged before it is valid
      # optional: lvgl_id, refresh_interval (30s), min_redraw_interval (500ms),
      #           heat_refresh_interval (15s), touchscreen_id (touch latency in the perf log),
      #           blitter (software | ppa; default software), present (false) with display_id
      #           internal_ram_floor (64kB; below it, warn and pause sprite builds; 0: off, D-064)
      #           heap_probe (counters | walk | off; default counters, D-067)
      # bisecting aids for the hosted Wi-Fi link (D-063), off by default:
      #           diagnostics (false), pulse_sprites (animated | still | off; default animated)
      # bisecting aids for the blue flash (D-067), default as shipped:
      #           heap_probe: walk, hosted_packet_pool (true), ui_storage (psram | system)
      # heat rebuilds and sprite frames on the other core (D-069), default true; false runs them
      # inline in loop() slices, as before (a bisecting aid):
      #           worker (true)

Build-time work lives in hearthmap_codegen.py (pure Python, unit-tested); this file is only the
ESPHome surface. See README.md for how core and ui_lvgl reach the build.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path

import esphome.codegen as cg
from esphome.components import display
from esphome.components import time as time_
from esphome.components.touchscreen import Touchscreen
from esphome.components.lvgl.defines import CONF_LVGL_ID, add_lv_use, get_lv_fonts_used
from esphome.components.lvgl.lvcode import LvglComponent
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID, CONF_TIME_ID, CONF_WATCHDOG_TIMEOUT
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority

from . import hearthmap_codegen as gen

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@abradner"]
DEPENDENCIES = ["api", "lvgl", "time"]

CONF_CONFIG = "config"
CONF_PHOTOS = "photos"
CONF_REFRESH_INTERVAL = "refresh_interval"
CONF_MIN_REDRAW_INTERVAL = "min_redraw_interval"
CONF_HEAT_REFRESH_INTERVAL = "heat_refresh_interval"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_BLITTER = "blitter"
CONF_PRESENT = "present"
CONF_DISPLAY_ID = "display_id"
CONF_DIAGNOSTICS = "diagnostics"
CONF_PULSE_SPRITES = "pulse_sprites"
CONF_INTERNAL_RAM_FLOOR = "internal_ram_floor"
CONF_HEAP_PROBE = "heap_probe"
CONF_HOSTED_PACKET_POOL = "hosted_packet_pool"
CONF_UI_STORAGE = "ui_storage"
CONF_WORKER = "worker"

# The component lives at <repo>/components/hearthmap; core/, ui_lvgl/ and schema/ are siblings
# of components/ in the same checkout. ESPHome clones the whole repo (depth 1) for a github://
# source, and a local source is a working tree, so they are present either way.
REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "schema" / "hearthmap.schema.json"

hearthmap_panel_ns = cg.esphome_ns.namespace("hearthmap_panel")
HearthmapComponent = hearthmap_panel_ns.class_("HearthmapComponent", cg.Component)
BlitterKind = hearthmap_panel_ns.enum("BlitterKind", is_class=True)
# `blitter: ppa` routes large pixel moves to the ESP32-P4's PPA (ppa_blitter.h, D-057); any other
# target, or a refused driver, falls back to plain C++ at runtime, so it only ever costs speed.
BLITTER_KINDS = {"software": BlitterKind.Software, "ppa": BlitterKind.Ppa}
PulseMode = cg.global_ns.namespace("hearthmap").namespace("ui").enum("PulseMode", is_class=True)
# `pulse_sprites: still` composes the sprites but never animates them; `off` never builds them.
# For bisecting the hosted Wi-Fi link fault (docs/decisions.md D-063), not for normal use.
PULSE_MODES = {"animated": PulseMode.Animated, "still": PulseMode.Still, "off": PulseMode.Off}
HeapProbe = hearthmap_panel_ns.enum("HeapProbe", is_class=True)
# What the once-a-second memory check reads (heap_watch.h, D-067). `walk` restores the
# largest-free-block walks, which hold interrupts off long enough to flash the panel blue; it
# is there to reproduce that, not for normal use. `off` leaves the internal-RAM floor blind.
HEAP_PROBES = {"counters": HeapProbe.Counters, "walk": HeapProbe.Walk, "off": HeapProbe.Off}
UI_STORAGES = ("psram", "system")


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


def _validate_present(config: dict) -> dict:
    if config[CONF_PRESENT] and CONF_DISPLAY_ID not in config:
        raise cv.Invalid("present: true needs display_id (the display LVGL draws to)", [CONF_PRESENT])
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
            # Optional: lets the perf log time a touch from the touchscreen's read to LVGL's press.
            cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(Touchscreen),
            cv.Optional(CONF_BLITTER, default="software"): cv.enum(BLITTER_KINDS, lower=True),
            # `present: true` lets small animated tiles reach the panel without an LVGL refresh
            # (display_presenter.h, D-058); needs the display LVGL draws to.
            cv.Optional(CONF_PRESENT, default=False): cv.boolean,
            cv.Optional(CONF_DISPLAY_ID): cv.use_id(display.Display),
            # A 10 s heap / SDIO / Wi-Fi / lwIP / task report on the UART (hosted_diag.h, D-063).
            cv.Optional(CONF_DIAGNOSTICS, default=False): cv.boolean,
            # A bare `off` is YAML false: read it as "off" (hearthmap_codegen.yaml_off).
            cv.Optional(CONF_PULSE_SPRITES, default="animated"): cv.All(
                gen.yaml_off, cv.enum(PULSE_MODES, lower=True)
            ),
            # Internal RAM below this: a rate-limited warning, and pulse sprite builds pause so
            # the hosted Wi-Fi driver keeps its DMA frames (internal_ram_guard.h, D-064). 0: off.
            cv.Optional(CONF_INTERNAL_RAM_FLOOR, default="64kB"): cv.All(
                cv.validate_bytes, cv.int_range(min=0, max=400_000)
            ),
            cv.Optional(CONF_HEAP_PROBE, default="counters"): cv.All(
                gen.yaml_off, cv.enum(HEAP_PROBES, lower=True)
            ),
            # Bisecting aids for the blue flash (D-067): `false` leaves esp-hosted's SDIO packet
            # pool at its own default (off); `system` keeps the UI's bulk storage on malloc.
            cv.Optional(CONF_HOSTED_PACKET_POOL, default=True): cv.boolean,
            cv.Optional(CONF_UI_STORAGE, default="psram"): cv.one_of(*UI_STORAGES, lower=True),
            # Heat rebuilds and sprite frames on a task on the other core (D-069); `false` runs
            # them inline in loop() slices, as before, for bisecting.
            cv.Optional(CONF_WORKER, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_document,
    _validate_present,
)


# LVGL widgets (and the flex layout) ui_lvgl uses from C++, and the built-in fonts its builtin_fonts() names
# (ui_lvgl/include/hearthmap/ui/fonts.h). LVGL compiles only what a build declares.
UI_WIDGETS = ("label", "line", "image", "arc", "flex")
UI_FONTS = tuple(f"montserrat_{px}" for px in (20, 22, 24, 26, 28, 30, 34))


def _check_watchdog(full_config: dict) -> None:
    floor_ms = gen.hosted_watchdog_floor_ms(full_config)
    if floor_ms is None:
        return
    wdt = full_config.get("esp32", {}).get(CONF_WATCHDOG_TIMEOUT)
    wdt_ms = int(wdt.total_milliseconds) if wdt is not None else 5000
    if wdt_ms < floor_ms:
        raise cv.Invalid(
            f"hearthmap: esp32 watchdog_timeout is {wdt_ms // 1000}s, but each Wi-Fi call to the "
            f"esp32_hosted co-processor may wait up to its RPC timeout, and a main-loop pass can meet "
            f"{gen.HOSTED_RPCS_PER_PASS} of them while the link is failing, so a silent co-processor resets "
            f"the panel. Set `esp32: watchdog_timeout: {-(-floor_ms // 1000)}s` or more (D-061)."
        )


def _note_hidden_warnings(full_config: dict) -> None:
    if gen.hosted_warnings_hidden(full_config):
        _LOGGER.warning(
            "hearthmap: esp32 framework log_level is below WARN, so esp-hosted's out-of-memory "
            "warnings (a Wi-Fi link dropping packets) are compiled out. Consider "
            "`esp32: framework: log_level: WARN` (D-063, D-064)."
        )


def _final_validate(config: dict) -> dict:
    # Done at validation time so it precedes lvgl's to_code, which emits the LV_USE_* and
    # LV_FONT_* defines from these sets.
    add_lv_use(*UI_WIDGETS)
    get_lv_fonts_used().update(UI_FONTS)
    full = fv.full_config.get()
    _check_watchdog(full)
    if not config[CONF_DIAGNOSTICS]:  # diagnostics opens those warnings itself
        _note_hidden_warnings(full)
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


def _diagnostics_sdkconfig() -> None:
    """What `diagnostics: true` needs from ESP-IDF and esp-hosted (D-063): esp-hosted's SDIO
    packet counters (read by hosted_diag.cpp; their own periodic INFO line stays below the
    runtime log level), FreeRTOS run-time stats for the task table, and a compiled log ceiling
    of INFO, so that esp-hosted's WARN lines exist for hosted_diag.cpp to let through at runtime
    (ESPHome compiles ESP-IDF's log at ERROR by default)."""
    from esphome.components.esp32 import add_idf_sdkconfig_option

    add_idf_sdkconfig_option("CONFIG_ESP_HOSTED_HOST_PKT_STATS", True)
    add_idf_sdkconfig_option("CONFIG_ESP_HOSTED_HOST_PKT_STATS_INTERVAL_SEC", 600)
    add_idf_sdkconfig_option("CONFIG_FREERTOS_USE_TRACE_FACILITY", True)
    add_idf_sdkconfig_option("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS", True)
    add_idf_sdkconfig_option("CONFIG_LOG_MAXIMUM_LEVEL_INFO", True)


def _hosted_mempool() -> None:
    """esp-hosted's SDIO packet pool (D-064): with it, every frame the P4 sends or reads comes
    from 31 blocks of 1536 bytes (about 47 KiB of DMA-capable internal RAM) reserved once when
    the link starts (the RX queue's 20 plus 11; esp-hosted 3.1.0, eh_host_bus_sdio.c), instead of
    a fresh heap_caps_malloc(1536, MALLOC_CAP_DMA) per frame that fails once internal RAM is
    short. A full pool drops a frame until one is freed; it never asks the heap. Set as a
    default, so a user's own sdkconfig_options value wins."""
    from esphome.components.esp32 import set_idf_sdkconfig_default

    set_idf_sdkconfig_default("CONFIG_ESP_HOSTED_HOST_SDIO_USE_MEMPOOL", True)


@coroutine_with_priority(CoroPriority.FINAL)
async def _psram_malloc() -> None:
    """Let plain malloc/new use PSRAM for large blocks. The psram component sets
    CONFIG_SPIRAM_USE_CAPS_ALLOC (PSRAM only through heap_caps_malloc), but ui_lvgl's
    core keeps its heat field and room mask in std::vectors (over 100 KiB on the panel), more than
    the P4's internal RAM can spare. With CONFIG_SPIRAM_USE_MALLOC, blocks above
    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KiB by default) go to PSRAM and small ones stay
    internal, which is why ui_lvgl's bulk storage does not rely on it: many small buffers would
    all land in internal RAM (D-064); they go through the memory port instead. Run at FINAL
    priority so it lands after the psram component's own setting."""
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
    cg.add(var.set_blitter_kind(config[CONF_BLITTER]))
    wdt = CORE.config.get("esp32", {}).get(CONF_WATCHDOG_TIMEOUT)
    if wdt is not None:
        cg.add(var.set_watchdog_ms(int(wdt.total_milliseconds)))
    cg.add(var.set_pulse_mode(config[CONF_PULSE_SPRITES]))
    cg.add(var.set_internal_ram_floor(config[CONF_INTERNAL_RAM_FLOOR]))
    cg.add(var.set_heap_probe(config[CONF_HEAP_PROBE]))
    if config[CONF_UI_STORAGE] != "psram":
        cg.add(var.set_ui_storage_psram(False))
    if not config[CONF_WORKER]:
        cg.add(var.set_worker_enabled(False))
    if config[CONF_DIAGNOSTICS]:
        cg.add_define("HEARTHMAP_DIAGNOSTICS")
        if CORE.using_toolchain_esp_idf:
            _diagnostics_sdkconfig()
    if config[CONF_PRESENT]:
        cg.add(var.set_present_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    if CONF_TOUCHSCREEN_ID in config:
        cg.add(var.set_touchscreen(await cg.get_variable(config[CONF_TOUCHSCREEN_ID])))
    # Subscribing to HA states from C++ needs the API's state-subscription code compiled in.
    cg.add_define("USE_API_HOMEASSISTANT_STATES")

    _write_build_tree(config)
    if CORE.is_esp32 and "psram" in CORE.config:
        CORE.add_job(_psram_malloc)
    pool = config[CONF_HOSTED_PACKET_POOL]
    if pool and CORE.using_toolchain_esp_idf and gen.hosted_sdio(CORE.config):
        _hosted_mempool()

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
