#!/usr/bin/env python3
"""Validate example configs and readings fixtures.

Checks, for every examples/<name>/:
  1. config.json against schema/hearthmap.schema.json (JSON Schema 2020-12);
  2. every readings/*.json against schema/readings.schema.json;
  3. the cross-references a schema cannot express: room and pin ids unique, every pin's room
     exists, and every entity a fixture mentions is bound by some pin in the config (a fixture
     naming an unknown entity is a typo that would silently test nothing). The entity check runs
     whenever config.json passed its own checks, including when it binds no entities at all
     (then every fixture entity is unknown); it is skipped only when config.json failed, which
     is already reported.

Semantic rules about values (band ordering, calibration width, polygon simplicity) are core's
validate() (core/include/hearthmap/config.h), not this script's: enforcement lives in one place.
This script is a CI aid for the examples, not a gate any config source may rely on.

Usage: python3 schema/validate.py [examples-dir ...]   (default: every directory in examples/)
Exit status: 0 when everything passes, 1 otherwise. Prints one line per file checked, so a
run that checked nothing is visible (and is itself an error).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent.parent
CONFIG_SCHEMA = ROOT / "schema" / "hearthmap.schema.json"
READINGS_SCHEMA = ROOT / "schema" / "readings.schema.json"


def load(path: Path):
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def schema_errors(validator: Draft202012Validator, doc) -> list[str]:
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    return [f"/{'/'.join(str(p) for p in e.absolute_path)}: {e.message}" for e in errors]


def reference_errors(config) -> tuple[list[str], set[str]]:
    errors: list[str] = []
    room_ids = [r["id"] for r in config.get("rooms", [])]
    pin_ids = [p["id"] for p in config.get("pins", [])]
    for kind, ids in (("room", room_ids), ("pin", pin_ids)):
        seen: set[str] = set()
        for i in ids:
            if i in seen:
                errors.append(f"duplicate {kind} id '{i}'")
            seen.add(i)
    rooms = set(room_ids)
    entities: set[str] = set()
    for n, pin in enumerate(config.get("pins", [])):
        if pin["room"] not in rooms:
            errors.append(f"/pins/{n}/room: unknown room '{pin['room']}'")
        entities.update(pin["entities"].values())
    return errors, entities


def check_example(directory: Path, config_v, readings_v) -> tuple[int, int]:
    """Returns (files checked, failures)."""
    checked = failures = 0
    config_path = directory / "config.json"
    if not config_path.exists():
        print(f"FAIL {config_path}: missing")
        return 0, 1
    config = load(config_path)
    errs = schema_errors(config_v, config)
    entities: set[str] = set()
    if not errs:
        ref_errs, entities = reference_errors(config)
        errs += ref_errs
    config_ok = not errs
    checked += 1
    failures += report(config_path, errs)

    for fixture_path in sorted((directory / "readings").glob("*.json")):
        fixture = load(fixture_path)
        errs = schema_errors(readings_v, fixture)
        if not errs and config_ok:
            unknown = sorted(set(fixture["readings"]) - entities)
            errs += [f"/readings/{e}: not bound by any pin in config.json" for e in unknown]
        checked += 1
        failures += report(fixture_path, errs)
    return checked, failures


def report(path: Path, errors: list[str]) -> int:
    rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
    if errors:
        print(f"FAIL {rel}")
        for e in errors:
            print(f"     {e}")
        return 1
    print(f"ok   {rel}")
    return 0


def main(argv: list[str]) -> int:
    config_schema = load(CONFIG_SCHEMA)
    readings_schema = load(READINGS_SCHEMA)
    Draft202012Validator.check_schema(config_schema)
    Draft202012Validator.check_schema(readings_schema)
    config_v = Draft202012Validator(config_schema)
    readings_v = Draft202012Validator(readings_schema)

    dirs = [Path(a).resolve() for a in argv] or sorted(
        p for p in (ROOT / "examples").iterdir() if p.is_dir()
    )
    checked = failures = 0
    for d in dirs:
        c, f = check_example(d, config_v, readings_v)
        checked += c
        failures += f
    if checked == 0:
        print("FAIL nothing was checked")
        return 1
    print(f"{checked} file(s) checked, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
