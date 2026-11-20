"""Shared fixtures for the hearthmap component's generator tests.

Run from the repo root:  python -m pytest components/hearthmap/tests
Needs components/hearthmap/tests/requirements.txt. The C++ tests also need a C++17 compiler
(c++, or $CXX).
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import pytest

COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
sys.path.insert(0, str(COMPONENT))  # import hearthmap_codegen without ESPHome

import hearthmap_codegen  # noqa: E402


@pytest.fixture(scope="session")
def gen():
    return hearthmap_codegen


@pytest.fixture(scope="session")
def schema() -> dict:
    return json.loads((REPO / "schema" / "hearthmap.schema.json").read_text(encoding="utf-8"))


@pytest.fixture(scope="session")
def _example() -> dict:
    return json.loads((REPO / "examples" / "flat" / "config.json").read_text(encoding="utf-8"))


@pytest.fixture
def example(_example) -> dict:
    return copy.deepcopy(_example)


@pytest.fixture
def minimal() -> dict:
    """The smallest schema-valid config: one room, no pins, every optional field omitted."""
    return {
        "version": 1,
        "canvas": {"width": 1280, "height": 800, "orientation": "landscape"},
        "rooms": [{"id": "room", "name": "Room", "polygon": [[0, 0], [1, 0], [1, 1]]}],
        "pins": [],
    }
