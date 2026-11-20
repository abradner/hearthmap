"""Generator output: defaults, present bits, bindings, literals, photos, the library copy."""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from PIL import Image


def _layer_block(src: str, layer: str) -> str:
    m = re.search(r"\{\n    LayerConfig &l = c\.layers\[index_of\(Layer::" + layer + r"\)\];\n(.*?)\n  \}", src, re.S)
    assert m, f"no block for {layer}"
    return m.group(1)


# ---- defaults --------------------------------------------------------------------------------


def test_omitted_fields_take_schema_defaults(gen, schema, minimal):
    src = gen.emit_config_source(minimal, schema, "m.json").source
    assert "c.temperature_unit = TemperatureUnit::Celsius;" in src
    assert "c.stale_after = Seconds{10800};" in src
    assert "c.default_layer = Layer::Temperature;" in src
    assert "c.heat.grid_cols = 48;" in src
    assert "c.heat.grid_rows = 40;" in src
    assert "c.heat.idw_power = 2.0;" in src
    assert "c.hud.max_items = 6;" in src
    assert "r.outdoor = false;" in src
    for layer in ("Temperature", "Humidity", "Soil", "Air"):
        block = _layer_block(src, layer)
        assert "l.enabled = true;" in block
        assert f"l.band = default_band(Layer::{layer});" in block  # band defaults stay in core
        assert "l.present = 0x00;" in block


def test_defaults_are_read_from_the_schema_not_this_generator(gen, schema, minimal):
    import copy

    s = copy.deepcopy(schema)
    s["properties"]["stale_after_s"]["default"] = 4321
    s["$defs"]["hud"]["properties"]["max_items"]["default"] = 3
    src = gen.emit_config_source(minimal, s, "m.json").source
    assert "c.stale_after = Seconds{4321};" in src
    assert "c.hud.max_items = 3;" in src


def test_omitted_map_area_is_the_whole_canvas(gen, schema, minimal):  # R14, D-023
    minimal["canvas"].update(width=1024, height=600)
    src = gen.emit_config_source(minimal, schema, "m.json").source
    assert "c.canvas.map_area = CanvasRect{0, 0, 1024, 600};" in src


def test_explicit_values_win_over_defaults(gen, schema, example):
    src = gen.emit_config_source(example, schema, "e.json").source
    assert "c.canvas.map_area = CanvasRect{40, 76, 864, 720};" in src
    assert "c.hud.max_items = 4;" in src
    assert src.count("r.outdoor = true;") == 1  # the balcony


# ---- present bits ----------------------------------------------------------------------------


@pytest.mark.parametrize(
    "field,bit",
    [("comfort_min", 0), ("comfort_max", 1), ("warn_below", 2), ("warn_above", 3),
     ("danger_below", 4), ("danger_above", 5), ("scale_min", 6), ("scale_max", 7)],
)
def test_each_written_band_field_sets_exactly_its_bit(gen, schema, minimal, field, bit):
    minimal["layers"] = {"humidity": {field: 50}}
    src = gen.emit_config_source(minimal, schema, "m.json").source
    block = _layer_block(src, "Humidity")
    assert f"l.present = 0x{1 << bit:02X};" in block
    assert f"l.band.{field} = 50.0;" in block
    assert "l.present = 0x00;" in _layer_block(src, "Temperature")


def test_null_threshold_counts_as_present_and_clears_it(gen, schema, minimal):
    minimal["layers"] = {"air": {"danger_above": None}}
    block = _layer_block(gen.emit_config_source(minimal, schema, "m.json").source, "Air")
    assert "l.band.danger_above = std::nullopt;" in block
    assert "l.present = 0x20;" in block


def test_enabled_is_not_a_band_field(gen, schema, minimal):
    minimal["layers"] = {"soil": {"enabled": False}}
    block = _layer_block(gen.emit_config_source(minimal, schema, "m.json").source, "Soil")
    assert "l.enabled = false;" in block
    assert "l.present = 0x00;" in block


def test_fahrenheit_with_all_eight_fields_is_all_bits(gen, schema, minimal):
    minimal["temperature_unit"] = "F"
    minimal["layers"] = {"temperature": {
        "comfort_min": 66, "comfort_max": 75, "warn_below": 61, "warn_above": 81,
        "danger_below": None, "danger_above": 90, "scale_min": 50, "scale_max": 93}}
    assert gen.check_schema(schema, minimal) == []
    src = gen.emit_config_source(minimal, schema, "m.json").source
    assert "c.temperature_unit = TemperatureUnit::Fahrenheit;" in src
    assert "l.present = 0xFF;" in _layer_block(src, "Temperature")


# ---- pins and bindings -----------------------------------------------------------------------


def test_bindings_follow_role_enum_order_not_json_order(gen, schema, example):
    pin = next(p for p in example["pins"] if p["id"] == "plant_fiddle")
    pin["entities"] = dict(reversed(list(pin["entities"].items())))
    src = gen.emit_config_source(example, schema, "e.json").source
    block = src[src.index('p.id = "plant_fiddle";'):]
    roles = re.findall(r"Role::(\w+)", block[: block.index("c.pins.push_back")])
    assert roles == ["Temperature", "Humidity", "Moisture", "Battery"]


def test_subscriptions_are_distinct_and_skip_reserved_roles(gen, schema, example):
    example["pins"][1]["entities"]["humidity"] = example["pins"][0]["entities"]["humidity"]
    out = gen.emit_config_source(example, schema, "e.json")
    assert len(out.entity_ids) == len(set(out.entity_ids))
    assert "sensor.example_basil_predicted_dry_at" not in out.entity_ids
    assert 'Role::PredictedDryAt, "sensor.example_basil_predicted_dry_at"' in out.source  # still bound
    assert f"kSubscribedEntityCount = {len(out.entity_ids)};" in out.source


def test_no_pins_still_emits_valid_cpp_tables(gen, schema, minimal):
    src = gen.emit_config_source(minimal, schema, "m.json").source
    assert "kSubscribedEntities[] = {{nullptr}};" in src
    assert "kSubscribedEntityCount = 0;" in src


def test_optional_pin_fields(gen, schema, example):
    src = gen.emit_config_source(example, schema, "e.json").source
    assert 'p.photo = std::string("monstera");' in src
    assert "p.stale_after = Seconds{21600};" in src
    assert "cal.happy_min = 25.0;" in src
    assert "cal.dry = 2900.0;" in src and "cal.wet = 1300.0;" in src  # inverted range kept


def test_validate_is_the_gate(gen, schema, minimal):
    src = gen.emit_config_source(minimal, schema, "m.json").source
    assert "result.issues = validate(c);" in src
    assert "if (usable(result.issues)) result.config = std::move(c);" in src


# ---- literals --------------------------------------------------------------------------------


@pytest.mark.parametrize(
    "s,expected",
    [("plain", '"plain"'), ('a"b\\c', '"a\\"b\\\\c"'), ("é", '"\\303\\251"'),
     ("new\nline", '"new\\012line"'), ("??=", '"\\077\\077="'), ("\x01" + "7", '"\\0017"')],
)
def test_cpp_string_escapes(gen, s, expected):
    assert gen.cpp_string(s) == expected


@pytest.mark.parametrize("x", [0, 1, -3, 0.1, 1e-9, 2900, 1.2345678901234567e300, 0.4167])
def test_cpp_double_round_trips(gen, x):
    assert float(gen.cpp_double(x)) == float(x)
    assert re.fullmatch(r"-?[0-9.]+(e[-+]?[0-9]+)?", gen.cpp_double(x))


def test_non_finite_numbers_are_refused(gen):
    with pytest.raises(gen.ConfigError):
        gen.cpp_double(float("inf"))


# ---- photos ----------------------------------------------------------------------------------


def test_rgb565_matches_core_truncation(gen):
    assert gen.rgb565(255, 255, 255) == 0xFFFF
    assert gen.rgb565(0, 0, 0) == 0
    assert gen.rgb565(0xF8, 0, 0) == 0xF800
    assert gen.rgb565(0, 0xFC, 0) == 0x07E0
    assert gen.rgb565(0, 0, 0xF8) == 0x001F
    assert gen.rgb565(0x07, 0x03, 0x07) == 0  # low bits truncated, not rounded


def test_photo_conversion_size_order_and_colour(gen):
    img = Image.new("RGB", (300, 200), (255, 0, 0))
    data = gen.to_rgb565_bytes(img, 16)
    assert len(data) == 16 * 16 * 2
    assert data[:2] == bytes((0x00, 0xF8))  # 0xF800 little-endian


def test_photo_is_centre_cropped(gen):
    img = Image.new("RGB", (30, 10), (0, 0, 255))
    img.paste((0, 255, 0), (10, 0, 20, 10))  # green middle third
    data = gen.to_rgb565_bytes(img, 10)
    assert set(data[i] | data[i + 1] << 8 for i in range(0, len(data), 2)) == {0x07E0}


def test_transparency_flattens_onto_disc_fill(gen):
    img = Image.new("RGBA", (4, 4), (255, 255, 255, 0))
    data = gen.to_rgb565_bytes(img, 4)
    v = gen.rgb565(*gen.PHOTO_BACKGROUND)
    assert data[:2] == bytes((v & 0xFF, v >> 8))


def test_named_photo_without_file_is_refused(gen, example, tmp_path):
    with pytest.raises(gen.ConfigError) as e:
        gen.check_photos(example, tmp_path)
    assert e.value.path.startswith("/pins/") and e.value.path.endswith("/photo")


def test_named_photos_without_a_directory_are_refused(gen, example):
    with pytest.raises(gen.ConfigError):
        gen.check_photos(example, None)


def test_no_photos_needed_when_none_named(gen, minimal):
    assert gen.check_photos(minimal, None) == {}


def test_example_photos_are_complete(gen, example):
    repo = Path(__file__).resolve().parents[3]
    found = gen.check_photos(example, repo / "examples" / "flat" / "photos")
    assert set(found) == set(gen.photo_ids(example))


def test_emit_photos_includes_placeholder_and_each_photo(gen, tmp_path):
    p = tmp_path / "fern.png"
    Image.new("RGB", (8, 8), (0, 255, 0)).save(p)
    header, src = gen.emit_photos({"fern": p}, size=8)
    assert "Placeholder" not in header + src  # ui_lvgl draws its own leaf for photo-less pins
    assert '{"fern", kPhoto0, 8},' in src
    assert "kPhotoCount = 1;" in src
    assert src.count("0xE0, 0x07") >= 64  # 8x8 green pixels, little-endian 0x07E0
    _, empty = gen.emit_photos({}, size=8)
    assert "kPhotoCount = 0;" in empty


# ---- library copy ----------------------------------------------------------------------------


def test_library_files_mapping(gen, tmp_path):
    (tmp_path / "core/include/hearthmap").mkdir(parents=True)
    (tmp_path / "core/include/hearthmap/config.h").write_text("//")
    (tmp_path / "core/src").mkdir()
    (tmp_path / "core/src/config.cpp").write_text("//")
    (tmp_path / "core/tests").mkdir()
    (tmp_path / "core/tests/config_test.cpp").write_text("//")
    (tmp_path / "core/src/notes.md").write_text("x")
    files = gen.library_files(tmp_path)
    (tmp_path / "host/src").mkdir(parents=True)
    (tmp_path / "host/src/summary.cpp").write_text("//")
    (tmp_path / "host/src/app.cpp").write_text("//")  # host-only: never copied
    files = gen.library_files(tmp_path)
    assert set(files) == {"hearthmap/config.h", "hearthmap_lib/core/config.cpp",
                          "hearthmap_lib/host/summary.cpp"}
    assert gen.public_include_roots(files) == {"hearthmap"}


def test_sync_tree_writes_changed_and_removes_stale_only_in_owned_dirs(gen, tmp_path):
    (tmp_path / "hearthmap_lib").mkdir()
    (tmp_path / "hearthmap_lib/old.cpp").write_text("old")
    (tmp_path / "esphome").mkdir()
    (tmp_path / "esphome/keep.cpp").write_text("not ours")
    written = gen.sync_tree(tmp_path, {"hearthmap_lib/new.cpp": b"new"}, {"hearthmap_lib"})
    assert written == ["hearthmap_lib/new.cpp"]
    assert not (tmp_path / "hearthmap_lib/old.cpp").exists()
    assert (tmp_path / "esphome/keep.cpp").read_text() == "not ours"
    assert gen.sync_tree(tmp_path, {"hearthmap_lib/new.cpp": b"new"}, {"hearthmap_lib"}) == []
