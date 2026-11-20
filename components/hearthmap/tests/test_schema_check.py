"""The build-time schema checker agrees with the reference validator (jsonschema).

ESPHome's image has no jsonschema, so the component carries its own small checker. These tests
hold it to the reference: every negative control in schema/test_schema.py must be rejected by it,
and on the example plus every mutation the two must give the same verdict.
"""

from __future__ import annotations

import copy
import importlib.util
import unittest
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parents[3]


class _Adapter:
    """Looks enough like a jsonschema validator for schema/test_schema.py's ConfigSchema cases."""

    def __init__(self, gen, schema):
        self.gen, self.schema = gen, schema

    class _Error:
        def __init__(self, path, message):
            self.path, self.message = path, message

        def __repr__(self):
            return f"{self.path or '/'}: {self.message}"

    def iter_errors(self, doc):
        # test_schema.py reads `.message` from each error, as jsonschema's errors have.
        return iter(self._Error(p, m) for p, m in self.gen.check_schema(self.schema, doc))


def _load_test_schema_module():
    spec = importlib.util.spec_from_file_location("hm_test_schema", REPO / "schema" / "test_schema.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_every_schema_negative_control_is_rejected(gen, schema):
    mod = _load_test_schema_module()
    mod.CONFIG_V = _Adapter(gen, schema)  # swap the reference validator for ours
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(mod.ConfigSchema)
    assert suite.countTestCases() > 10, "test_schema.py's config cases were not found"
    result = unittest.TextTestRunner(verbosity=0).run(suite)
    failures = [str(t) for t, _ in result.failures + result.errors]
    assert result.wasSuccessful(), failures


def _mutations(example):
    """Single-field mutations covering every keyword the checker implements."""
    def m(fn):
        d = copy.deepcopy(example)
        fn(d)
        return d

    pin0 = lambda d: d["pins"][0]  # noqa: E731
    yield m(lambda d: None)
    yield m(lambda d: d.update(version=2))  # const
    yield m(lambda d: d.update(temperature_unit="K"))  # enum
    yield m(lambda d: d.update(stale_after_s=0))  # minimum
    yield m(lambda d: d.update(stale_after_s=1.5))  # integer type
    yield m(lambda d: d.update(stale_after_s=2.0))  # 2.0 is an integer in JSON Schema
    yield m(lambda d: d.update(stale_after_s=True))  # bool is not an integer
    yield m(lambda d: d["canvas"].update(width=5000))  # maximum
    yield m(lambda d: d.update(heat={"idw_power": 0}))  # exclusiveMinimum
    yield m(lambda d: d.update(heat={"idw_power": 8}))
    yield m(lambda d: d.update(rooms=[]))  # minItems
    yield m(lambda d: d.update(rooms=d["rooms"] * 10))  # maxItems (70 > 64)
    yield m(lambda d: d["rooms"][0].update(id="Living"))  # pattern
    yield m(lambda d: d["rooms"][0].update(name=""))  # minLength
    yield m(lambda d: d["rooms"][0].update(name="x" * 49))  # maxLength
    yield m(lambda d: d["rooms"][0]["polygon"][0].append(0.5))  # items: false after prefixItems
    yield m(lambda d: d["rooms"][0]["polygon"].__setitem__(0, [1.5, 0]))  # prefixItems range
    yield m(lambda d: pin0(d).update(entities={}))  # minProperties
    yield m(lambda d: pin0(d).update(colour="red"))  # additionalProperties false
    yield m(lambda d: pin0(d).pop("room"))  # required
    yield m(lambda d: pin0(d).update(kind="plant"))  # if/then: plant needs calibration
    yield m(lambda d: d.update(layers={"temperature": {"warn_below": None}}))  # nullable threshold
    yield m(lambda d: d.update(layers={"temperature": {"comfort_min": None}}))  # not nullable
    yield m(lambda d: d.update(temperature_unit="F"))  # top-level if/then (Fahrenheit)


def test_verdicts_match_reference_validator(gen, schema, example):
    ref = Draft202012Validator(schema)
    cases = list(_mutations(example))
    assert len(cases) > 20
    for i, doc in enumerate(cases):
        ours = gen.check_schema(schema, doc) == []
        theirs = ref.is_valid(doc)
        assert ours == theirs, f"case {i}: ours={ours} reference={theirs}"
    # Both verdicts occur, so the comparison is not vacuous.
    verdicts = {gen.check_schema(schema, d) == [] for d in cases}
    assert verdicts == {True, False}


def test_unknown_keyword_fails_loudly(gen, schema, example):
    s = copy.deepcopy(schema)
    s["properties"]["version"]["multipleOf"] = 1
    with pytest.raises(gen.SchemaUnsupported):
        gen.check_schema(s, example)


def test_error_paths_are_json_pointers(gen, schema, example):
    example["pins"][3]["position"] = [0.5, 2]
    errors = gen.check_schema(schema, example)
    assert ("/pins/3/position/1", "must be <= 1") in errors


def test_adapter_errors_carry_a_message(gen, schema):
    errors = list(_Adapter(gen, schema).iter_errors({"version": 2}))
    assert errors and all(isinstance(e.message, str) for e in errors)
