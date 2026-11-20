"""calibration.schema.json: the committed example validates, and mutations are rejected."""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

EXAMPLE = json.loads((Path(__file__).resolve().parent.parent / "examples" / "calibration.json").read_text(encoding="utf-8"))


def ok_index():
    return next(i for i, s in enumerate(EXAMPLE["sensors"]) if s["status"] == "ok")


def failed_index():
    return next(i for i, s in enumerate(EXAMPLE["sensors"]) if s["status"] != "ok")


def test_example_is_valid(calibration_validator):
    assert list(calibration_validator.iter_errors(EXAMPLE)) == []


def _set(path, value):
    def mutate(doc):
        *head, last = path
        target = doc
        for k in head:
            target = target[k]
        if value is KeyError:
            del target[last]
        else:
            target[last] = value

    return mutate


OK, BAD = ok_index(), failed_index()
MUTATIONS = {
    "wrong version": _set(["version"], 2),
    "missing sensors": _set(["sensors"], KeyError),
    "unknown top-level key": _set(["extra"], 1),
    "ok without calibration": _set(["sensors", OK, "calibration"], KeyError),
    "ok with a message": _set(["sensors", OK, "message"], "x"),
    "ok with confidence none": _set(["sensors", OK, "confidence"], "none"),
    "calibration missing wet": _set(["sensors", OK, "calibration", "wet"], KeyError),
    "calibration missing happy_min": _set(["sensors", OK, "calibration", "happy_min"], KeyError),
    "calibration non-numeric dry": _set(["sensors", OK, "calibration", "dry"], "0"),
    "calibration unknown key": _set(["sensors", OK, "calibration", "mid"], 3),
    "failed with a calibration": _set(["sensors", BAD, "calibration"], {"dry": 0, "wet": 1, "happy_min": 0, "happy_max": 1}),
    "failed without a message": _set(["sensors", BAD, "message"], KeyError),
    "failed with confidence": _set(["sensors", BAD, "confidence"], "low"),
    "unknown status": _set(["sensors", OK, "status"], "maybe"),
    "negative events": _set(["sensors", OK, "watering_events"], -1),
    "zero drying rate": _set(["sensors", OK, "drying_rate_per_day"], 0),
    "bad polarity": _set(["sensors", OK, "polarity"], "sideways"),
    "bad entity id": _set(["sensors", OK, "entity_id"], "Sensor Example"),
    "warning without code": _set(["sensors", OK, "warnings"], [{"message": "x"}]),
}


@pytest.mark.parametrize("name", list(MUTATIONS))
def test_mutation_is_rejected(name, calibration_validator):
    doc = copy.deepcopy(EXAMPLE)
    MUTATIONS[name](doc)
    assert list(calibration_validator.iter_errors(doc)), f"schema accepted: {name}"
