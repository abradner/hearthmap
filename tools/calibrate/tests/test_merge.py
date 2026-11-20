"""Merging proposals into a config's soil and plant pins."""

from __future__ import annotations

import copy

from hearthmap_calibrate.merge import merge_config


def proposal(entity, dry, wet, hmin=None, hmax=None, status="ok", confidence="high"):
    if status != "ok":
        return {"entity_id": entity, "status": status, "message": f"cannot calibrate: {status}", "confidence": "none"}
    cal = {"dry": dry, "wet": wet, "happy_min": hmin if hmin is not None else dry + 0.3 * (wet - dry), "happy_max": hmax if hmax is not None else wet}
    return {"entity_id": entity, "status": "ok", "calibration": cal, "confidence": confidence}


def calib(*sensors):
    return {"version": 1, "sensors": list(sensors)}


def pin(config, pin_id):
    return next(p for p in config["pins"] if p["id"] == pin_id)


def by_subject(actions):
    return {a.subject: a.text for a in actions}


def test_updates_matching_pins_and_never_mutates_the_input(example_config):
    before = copy.deepcopy(example_config)
    out, actions = merge_config(example_config, calib(proposal("sensor.example_basil_moisture", 0.0, 50.0)))
    assert example_config == before
    assert pin(out, "plant_basil")["calibration"] == {"dry": 0.0, "wet": 50.0, "happy_min": 15.0, "happy_max": 50.0}
    assert by_subject(actions)["plant_basil"].startswith("updated: dry/wet 0..55 -> 0.0..50.0")
    # Everything else is untouched.
    for p_before, p_after in zip(before["pins"], out["pins"]):
        if p_before["id"] != "plant_basil":
            assert p_before == p_after


def test_soil_pins_are_merged_too(example_config):
    out, _ = merge_config(example_config, calib(proposal("sensor.example_planter_moisture", 2.0, 48.0)))
    assert pin(out, "soil_planter")["calibration"]["wet"] == 48.0


def test_existing_happy_range_is_kept_when_it_still_fits(example_config):
    # fiddle has dry 10, wet 70, happy 25..60
    out, actions = merge_config(example_config, calib(proposal("sensor.example_fiddle_moisture", 12.0, 66.0)))
    assert pin(out, "plant_fiddle")["calibration"] == {"dry": 12.0, "wet": 66.0, "happy_min": 25, "happy_max": 60}
    assert "existing happy range kept" in by_subject(actions)["plant_fiddle"]


def test_existing_happy_range_that_no_longer_fits_is_replaced(example_config):
    out, actions = merge_config(example_config, calib(proposal("sensor.example_fiddle_moisture", 30.0, 55.0)))
    cal = pin(out, "plant_fiddle")["calibration"]
    assert (cal["happy_min"], cal["happy_max"]) == (37.5, 55.0)
    assert "no longer fits" in by_subject(actions)["plant_fiddle"]


def test_one_sided_existing_happy_is_kept_alone(example_config):
    # fern: dry 15, wet 80, happy_min 40 only. Keeping it must not add a default happy_max.
    out, _ = merge_config(example_config, calib(proposal("sensor.example_fern_moisture", 10.0, 75.0)))
    assert pin(out, "plant_fern")["calibration"] == {"dry": 10.0, "wet": 75.0, "happy_min": 40}


def test_replace_happy_overrides_a_fitting_range(example_config):
    out, _ = merge_config(example_config, calib(proposal("sensor.example_fiddle_moisture", 12.0, 66.0)), replace_happy=True)
    assert pin(out, "plant_fiddle")["calibration"]["happy_min"] == 12.0 + 0.3 * 54.0


def test_inverted_ranges_check_happy_fit_in_their_own_direction(example_config):
    p = pin(example_config, "plant_pothos")
    p["calibration"] = {"dry": 2900, "wet": 1300, "happy_min": 2400, "happy_max": 1500}
    out, actions = merge_config(example_config, calib(proposal("sensor.example_pothos_moisture_raw", 2880.0, 1370.0)))
    assert pin(out, "plant_pothos")["calibration"] == {"dry": 2880.0, "wet": 1370.0, "happy_min": 2400, "happy_max": 1500}
    # And a happy_min wetter than happy_max is not kept.
    p["calibration"] = {"dry": 2900, "wet": 1300, "happy_min": 1500, "happy_max": 2400}
    out, _ = merge_config(example_config, calib(proposal("sensor.example_pothos_moisture_raw", 2880.0, 1370.0)))
    assert pin(out, "plant_pothos")["calibration"]["happy_min"] != 1500


def test_failed_proposals_leave_the_pin_alone(example_config):
    before = pin(example_config, "plant_lavender")["calibration"]
    out, actions = merge_config(example_config, calib(proposal("sensor.example_lavender_moisture", 0, 0, status="no_variation")))
    assert pin(out, "plant_lavender")["calibration"] == before
    assert by_subject(actions)["plant_lavender"] == "unchanged: cannot calibrate: no_variation"


def test_unused_proposals_and_unproposed_pins_are_reported(example_config):
    _, actions = merge_config(example_config, calib(proposal("sensor.example_nowhere_moisture", 1.0, 9.0)))
    acts = by_subject(actions)
    assert acts["sensor.example_nowhere_moisture"].startswith("not merged")
    assert acts["plant_basil"] == "unchanged: no proposal for sensor.example_basil_moisture"


def test_other_kinds_never_get_a_calibration(example_config):
    # Moisture is only valid on soil and plant pins (ruling R1); a stray binding must be skipped.
    climate = next(p for p in example_config["pins"] if p["kind"] == "temperature")
    climate["entities"]["moisture"] = "sensor.example_basil_moisture"
    out, actions = merge_config(example_config, calib(proposal("sensor.example_basil_moisture", 0.0, 50.0)))
    assert "calibration" not in pin(out, climate["id"])
    assert by_subject(actions)[climate["id"]].startswith("skipped: kind 'temperature'")
    assert "calibration" in pin(out, "plant_basil")


def test_merged_example_still_validates(example_config, config_validator):
    doc = calib(
        proposal("sensor.example_basil_moisture", 0.0, 50.0),
        proposal("sensor.example_pothos_moisture_raw", 2884.0, 1372.0),
        proposal("sensor.example_fiddle_moisture", 30.0, 55.0),
    )
    out, _ = merge_config(example_config, doc)
    assert list(config_validator.iter_errors(out)) == []
