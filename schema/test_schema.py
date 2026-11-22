#!/usr/bin/env python3
"""Negative controls for the schemas: each case mutates the valid example and must be rejected.

Without these, a schema that accepted everything would pass schema/validate.py too.
Run: python3 schema/test_schema.py   (stdlib unittest; needs schema/requirements.txt)
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "schema"))  # for `import validate`


def load(rel: str):
    return json.loads((ROOT / rel).read_text(encoding="utf-8"))


CONFIG_V = Draft202012Validator(load("schema/hearthmap.schema.json"))
READINGS_V = Draft202012Validator(load("schema/readings.schema.json"))
EXAMPLE = load("examples/flat/config.json")
FIXTURE = load("examples/flat/readings/all-ok.json")


# Every temperature band field, in Fahrenheit; null where the schema allows null.
FAHRENHEIT_BAND = {
    "comfort_min": 66, "comfort_max": 75,
    "warn_below": 61, "warn_above": 81,
    "danger_below": None, "danger_above": 90,
    "scale_min": 50, "scale_max": 93,
}


def pin(config, pin_id):
    return next(p for p in config["pins"] if p["id"] == pin_id)


class ConfigSchema(unittest.TestCase):
    def assert_valid(self, doc):
        errors = list(CONFIG_V.iter_errors(doc))
        self.assertEqual(errors, [], [e.message for e in errors])

    def assert_invalid(self, mutate):
        doc = copy.deepcopy(EXAMPLE)
        mutate(doc)
        self.assertTrue(list(CONFIG_V.iter_errors(doc)), "mutation was accepted")

    def test_example_is_valid(self):  # positive control
        self.assert_valid(EXAMPLE)

    def test_wrong_version(self):
        self.assert_invalid(lambda d: d.update(version=2))

    def test_unknown_top_level_field(self):
        self.assert_invalid(lambda d: d.update(colour="blue"))

    def test_plant_without_calibration(self):
        self.assert_invalid(lambda d: pin(d, "plant_basil").pop("calibration"))

    def test_plant_without_moisture(self):
        def m(d):
            p = pin(d, "plant_basil")
            p["entities"] = {"temperature": "sensor.example_x"}
        self.assert_invalid(m)

    def test_temperature_pin_with_calibration(self):
        self.assert_invalid(
            lambda d: pin(d, "clim_kitchen").update(calibration={"dry": 0, "wet": 1}))

    def test_temperature_pin_with_photo(self):
        self.assert_invalid(lambda d: pin(d, "clim_kitchen").update(photo="kitchen"))

    def test_air_quality_without_co2(self):
        self.assert_invalid(
            lambda d: pin(d, "air_living").update(entities={"pm25": "sensor.example_p"}))

    def test_unknown_role(self):
        self.assert_invalid(
            lambda d: pin(d, "clim_kitchen")["entities"].update(lux="sensor.example_lux"))

    def test_bad_entity_id(self):
        self.assert_invalid(
            lambda d: pin(d, "clim_kitchen")["entities"].update(temperature="Kitchen Temp"))

    def test_position_outside_unit_square(self):
        self.assert_invalid(lambda d: pin(d, "clim_kitchen").update(position=[1.2, 0.5]))

    def test_position_with_three_coordinates(self):
        self.assert_invalid(lambda d: pin(d, "clim_kitchen").update(position=[0.1, 0.2, 0.3]))

    def test_polygon_with_two_vertices(self):
        self.assert_invalid(lambda d: d["rooms"][0].update(polygon=[[0, 0], [1, 1]]))

    def test_bad_id(self):
        self.assert_invalid(lambda d: d["rooms"][0].update(id="Living Room"))

    def test_unknown_kind(self):
        self.assert_invalid(lambda d: pin(d, "clim_kitchen").update(kind="light"))

    def test_hud_cap_zero(self):
        self.assert_invalid(lambda d: d.update(hud={"max_items": 0}))

    def test_unknown_layer(self):
        self.assert_invalid(lambda d: d["layers"].update(noise={"warn_above": 60}))

    # Moisture binds only on soil and plant pins (docs/decisions.md D-018).
    def test_moisture_on_temperature_pin(self):
        self.assert_invalid(
            lambda d: pin(d, "clim_kitchen")["entities"].update(moisture="sensor.example_m"))

    def test_moisture_on_humidity_pin(self):
        self.assert_invalid(
            lambda d: pin(d, "hum_bathroom")["entities"].update(moisture="sensor.example_m"))

    def test_moisture_on_air_quality_pin(self):
        self.assert_invalid(
            lambda d: pin(d, "air_living")["entities"].update(moisture="sensor.example_m"))

    # Size caps (D-017).
    def test_more_than_64_rooms(self):
        def m(d):
            template = d["rooms"][0]
            d["rooms"] = [dict(template, id=f"room_{i}") for i in range(65)]
        self.assert_invalid(m)

    def test_64_rooms_is_allowed(self):  # positive control for the cap's edge
        doc = copy.deepcopy(EXAMPLE)
        template = doc["rooms"][0]
        doc["rooms"] = [dict(template, id=f"room_{i}") for i in range(64)]
        doc["pins"] = []
        self.assert_valid(doc)

    def test_more_than_256_pins(self):
        def m(d):
            template = pin(d, "clim_kitchen")
            d["pins"] = [dict(template, id=f"pin_{i}") for i in range(257)]
        self.assert_invalid(m)

    # Fahrenheit needs every temperature band field set (D-011).
    def test_fahrenheit_without_temperature_layer(self):
        self.assert_invalid(lambda d: d.update(temperature_unit="F"))

    def test_fahrenheit_without_layers(self):
        def m(d):
            d["temperature_unit"] = "F"
            d.pop("layers")
        self.assert_invalid(m)

    def test_fahrenheit_with_partial_band(self):
        def m(d):
            d["temperature_unit"] = "F"
            band = dict(FAHRENHEIT_BAND)
            band.pop("scale_max")
            d["layers"]["temperature"] = band
        self.assert_invalid(m)

    def test_fahrenheit_with_full_band_is_allowed(self):  # positive control
        doc = copy.deepcopy(EXAMPLE)
        doc["temperature_unit"] = "F"
        doc["layers"]["temperature"] = dict(FAHRENHEIT_BAND, enabled=True)
        self.assert_valid(doc)

    def test_celsius_without_temperature_layer_is_allowed(self):  # the rule is F-only
        doc = copy.deepcopy(EXAMPLE)
        doc["temperature_unit"] = "C"
        doc["layers"].pop("temperature", None)
        self.assert_valid(doc)

    def test_map_rotation_quarter_turns_are_allowed(self):  # positive control
        for degrees in (0, 90, 180, 270):
            doc = copy.deepcopy(EXAMPLE)
            doc["canvas"]["map_rotation"] = degrees
            self.assert_valid(doc)

    def test_map_rotation_45(self):
        self.assert_invalid(lambda d: d["canvas"].update(map_rotation=45))

    def test_map_rotation_other_values(self):
        for bad in (-90, 360, 1, 90.5, "90", None):
            with self.subTest(bad=bad):
                self.assert_invalid(lambda d, b=bad: d["canvas"].update(map_rotation=b))

    def test_threshold_null_is_allowed(self):
        doc = copy.deepcopy(EXAMPLE)
        doc["layers"]["temperature"] = {"warn_below": None, "danger_below": None}
        self.assert_valid(doc)


class ReadingsSchema(unittest.TestCase):
    def assert_invalid(self, mutate):
        doc = copy.deepcopy(FIXTURE)
        mutate(doc)
        self.assertTrue(list(READINGS_V.iter_errors(doc)), "mutation was accepted")

    def test_fixture_is_valid(self):  # positive control
        self.assertEqual(list(READINGS_V.iter_errors(FIXTURE)), [])

    def test_missing_now(self):
        self.assert_invalid(lambda d: d.pop("now"))

    def test_timestamp_without_offset(self):
        self.assert_invalid(lambda d: d.update(now="2026-01-15T09:00:00"))

    def test_string_value_other_than_ha_states(self):
        def m(d):
            first = next(iter(d["readings"].values()))
            first["value"] = "21.5"
        self.assert_invalid(m)

    def test_missing_last_updated(self):
        def m(d):
            next(iter(d["readings"].values())).pop("last_updated")
        self.assert_invalid(m)


class ValidateScript(unittest.TestCase):
    """schema/validate.py's own cross-check: a fixture naming an unbound entity fails."""

    def run_on(self, config, fixture):
        import tempfile

        import validate

        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "readings").mkdir()
            (d / "config.json").write_text(json.dumps(config), encoding="utf-8")
            (d / "readings" / "f.json").write_text(json.dumps(fixture), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                return validate.check_example(d, CONFIG_V, READINGS_V)

    def test_example_passes(self):  # positive control
        self.assertEqual(self.run_on(EXAMPLE, FIXTURE), (2, 0))

    def test_fixture_entity_unknown_to_config(self):
        fixture = copy.deepcopy(FIXTURE)
        fixture["readings"]["sensor.example_typo"] = next(iter(FIXTURE["readings"].values()))
        self.assertEqual(self.run_on(EXAMPLE, fixture), (2, 1))

    def test_config_binding_no_entities_still_cross_checks(self):
        config = copy.deepcopy(EXAMPLE)
        config["pins"] = []
        self.assertEqual(self.run_on(config, FIXTURE), (2, 1))


if __name__ == "__main__":
    unittest.main(verbosity=1)
