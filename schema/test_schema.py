#!/usr/bin/env python3
"""Negative controls for the schemas: each case mutates the valid example and must be rejected.

Without these, a schema that accepted everything would pass schema/validate.py too.
Run: python3 schema/test_schema.py   (stdlib unittest; needs schema/requirements.txt)
"""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent.parent


def load(rel: str):
    return json.loads((ROOT / rel).read_text(encoding="utf-8"))


CONFIG_V = Draft202012Validator(load("schema/hearthmap.schema.json"))
READINGS_V = Draft202012Validator(load("schema/readings.schema.json"))
EXAMPLE = load("examples/flat/config.json")
FIXTURE = load("examples/flat/readings/all-ok.json")


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


if __name__ == "__main__":
    unittest.main(verbosity=1)
