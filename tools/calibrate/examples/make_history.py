#!/usr/bin/env python3
"""Regenerate examples/history.json: synthetic history for the fictional flat's soil sensors.

Every series is invented (tests/synth.py); entity ids are the example flat's. A test checks the
committed file matches this script's output, so edit the script, not the JSON.
Run: python3 tools/calibrate/examples/make_history.py > tools/calibrate/examples/history.json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent), str(HERE.parent / "tests")]

from hearthmap_calibrate.history import History, Sample, history_to_json  # noqa: E402
from synth import change_only, constant, invert, only_dries, sawtooth, with_gaps, with_outliers  # noqa: E402


def build() -> History:
    def clip0(samples):
        return [Sample(s.t, max(0.0, s.v)) for s in samples]

    sensors = {
        # An ordinary percentage probe: weekly watering, noise, a few outages and a spike.
        "sensor.example_monstera_moisture": change_only(
            with_outliers(with_gaps(sawtooth(dry=6, wet=58, noise=0.8, seed=11, step_hours=2), every=60, length=4), [70], 97.0)
        ),
        # The motivating case: watered late, sits at 0% for days first.
        "sensor.example_basil_moisture": change_only(clip0(sawtooth(dry=-35, wet=52, period_days=9, days=36, step_hours=2))),
        # A raw ADC probe that reads lower when wet.
        "sensor.example_pothos_moisture_raw": change_only(
            invert(sawtooth(dry=10, wet=60, noise=0.3, seed=5, period_days=6, step_hours=2), scale=32, offset=3220), resolution=4
        ),
        # Never watered in the window: it only dries.
        "sensor.example_snake_moisture": change_only(only_dries(start=38, end=4, days=28, step_hours=2)),
        # Stuck at exactly 0: cannot be calibrated.
        "sensor.example_lavender_moisture": change_only(constant(0.0, days=28, step_hours=2)),
    }
    units = {e: "%" for e in sensors if not e.endswith("_raw")}
    return History(sensors=sensors, units=units)


if __name__ == "__main__":
    sys.stdout.write(json.dumps(history_to_json(build()), indent=1) + "\n")
