from __future__ import annotations

import json
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator
from referencing import Registry, Resource

TOOL = Path(__file__).resolve().parent.parent
ROOT = TOOL.parent.parent


def _load(rel: str):
    return json.loads((ROOT / rel).read_text(encoding="utf-8"))


@pytest.fixture(scope="session")
def config_schema():
    return _load("schema/hearthmap.schema.json")


@pytest.fixture(scope="session")
def calibration_validator(config_schema):
    """calibration.schema.json refers to hearthmap.schema.json's calibration by relative $ref."""
    schema = _load("schema/calibration.schema.json")
    registry = Registry().with_resources(
        [(s["$id"], Resource.from_contents(s)) for s in (schema, config_schema)]
    )
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema, registry=registry)


@pytest.fixture(scope="session")
def config_validator(config_schema):
    return Draft202012Validator(config_schema)


@pytest.fixture
def example_config():
    return _load("examples/flat/config.json")
