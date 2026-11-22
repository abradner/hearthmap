#!/usr/bin/env python3
"""Check that core's compiled-in defaults agree with what the schema documents.

Three copies of the same numbers exist (docs/decisions.md D-007):
  1. core: default_band() and Config's member initialisers (config.h), printed by
     hearthmap_dump_defaults;
  2. schema/README.md: the band defaults table and the top-level field tables;
  3. schema/hearthmap.schema.json: its "default" keywords (and map_area's description).
This script fails when any two disagree. --self-test plants a mismatch in each source in turn
and fails unless every planted mismatch is caught (a check counts only once seen to fail).

Run by ctest (core/tests/CMakeLists.txt). Standard library only.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
import sys

LAYERS = ("temperature", "humidity", "soil", "air")
BAND_FIELDS = (
    "comfort_min",
    "comfort_max",
    "warn_below",
    "warn_above",
    "danger_below",
    "danger_above",
    "scale_min",
    "scale_max",
)

# Every "default" keyword in the schema, by JSON pointer, and the key it is compared with in
# core's dump. A default added to the schema without a line here fails the check, so a new
# default cannot slip in unchecked.
SCHEMA_DEFAULTS = {
    "/properties/temperature_unit": "temperature_unit",
    "/properties/stale_after_s": "stale_after_s",
    "/$defs/room/properties/outdoor": "room_outdoor",
    "/$defs/layer/properties/enabled": "layers_enabled",
    "/$defs/layers/properties/default": "default_layer",
    "/$defs/hud/properties/max_items": "hud_max_items",
    "/$defs/heat/properties/grid_cols": "grid_cols",
    "/$defs/heat/properties/grid_rows": "grid_rows",
    "/$defs/heat/properties/idw_power": "idw_power",
    "/$defs/canvas/properties/map_rotation": "map_rotation",
}


def _num(text: str) -> float | None:
    text = text.strip()
    if text == "none":
        return None
    return float(text)


def parse_readme_bands(readme: str) -> dict[str, dict[str, float | None]]:
    """The band defaults table: | `layer` | domain | comfort | warn b/a | danger b/a | scale |."""
    bands: dict[str, dict[str, float | None]] = {}
    in_table = False
    for line in readme.splitlines():
        if line.startswith("| Layer | Domain | comfort |"):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            break
        cells = _cells(line)
        if set(cells[0]) <= {"-", " "}:
            continue  # the |---| separator row
        name = cells[0].strip("`")
        comfort = re.fullmatch(r"(-?[\d.]+)\s*[–-]\s*(-?[\d.]+)", cells[2])
        scale = re.fullmatch(r"(-?[\d.]+)\s*[–-]\s*(-?[\d.]+)", cells[5])
        warn = cells[3].split("/")
        danger = cells[4].split("/")
        if not comfort or not scale or len(warn) != 2 or len(danger) != 2:
            raise ValueError(f"cannot parse band row: {line!r}")
        bands[name] = {
            "comfort_min": float(comfort.group(1)),
            "comfort_max": float(comfort.group(2)),
            "warn_below": _num(warn[0]),
            "warn_above": _num(warn[1]),
            "danger_below": _num(danger[0]),
            "danger_above": _num(danger[1]),
            "scale_min": float(scale.group(1)),
            "scale_max": float(scale.group(2)),
        }
    return bands


def _cells(line: str) -> list[str]:
    """A markdown table row's cells; an escaped pipe (\\|) inside a cell does not split it."""
    return [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]


def _row(readme: str, field: str) -> str:
    for line in readme.splitlines():
        if line.startswith(f"| `{field}` |"):
            return line
    raise ValueError(f"README has no table row for `{field}`")


def _band_row(readme: str, layer: str) -> str:
    """A row of the band defaults table (the roles table also has rows named `temperature`)."""
    start = readme.index("| Layer | Domain | comfort |")
    for line in readme[start:].splitlines():
        if line.startswith(f"| `{layer}` |"):
            return line
    raise ValueError(f"band table has no row for `{layer}`")


def _default_in(text: str, key: str | None = None) -> str:
    """The first `value` after 'default' (optionally after `key`) in a README cell."""
    pattern = (rf"`{re.escape(key)}`[^`]*?default `([^`]+)`" if key else r"`([^`]+)`")
    m = re.search(pattern, text)
    if not m:
        raise ValueError(f"no default for {key or 'row'} in {text!r}")
    return m.group(1)


def parse_readme_scalars(readme: str) -> dict[str, object]:
    out: dict[str, object] = {}
    stale_cells = _cells(_row(readme, "stale_after_s"))
    out["stale_after_s"] = int(_default_in(stale_cells[2]))
    unit_cells = _cells(_row(readme, "temperature_unit"))
    out["temperature_unit"] = _default_in(unit_cells[2]).strip('"')
    hud = _row(readme, "hud")
    out["hud_max_items"] = int(_default_in(hud, "max_items"))
    heat = _row(readme, "heat")
    out["grid_cols"] = int(_default_in(heat, "grid_cols"))
    out["grid_rows"] = int(_default_in(heat, "grid_rows"))
    out["idw_power"] = float(_default_in(heat, "idw_power"))
    # map_area defaults to the whole canvas (docs/decisions.md D-023): documented symbolically.
    area_cells = _cells(_row(readme, "map_area"))
    out["map_area_whole_canvas"] = _default_in(area_cells[2]) == "{0, 0, width, height}"
    out["map_rotation"] = int(_default_in(_cells(_row(readme, "map_rotation"))[2]))
    m = re.search(r"`default` \([^)]*default `([a-z]+)`\)", readme)
    if not m:
        raise ValueError("README does not state the default layer")
    out["default_layer"] = m.group(1)
    m = re.search(r"\| `enabled` \| Default `(true|false)`", readme)
    if not m:
        raise ValueError("README does not state the layer `enabled` default")
    out["layers_enabled"] = m.group(1) == "true"
    return out


def collect_schema_defaults(node: object, pointer: str = "") -> dict[str, object]:
    found: dict[str, object] = {}
    if isinstance(node, dict):
        if "default" in node and not isinstance(node["default"], (dict, list)):
            found[pointer] = node["default"]
        for k, v in node.items():
            found.update(collect_schema_defaults(v, f"{pointer}/{k}"))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            found.update(collect_schema_defaults(v, f"{pointer}/{i}"))
    return found


def schema_map_area_whole_canvas(schema: dict) -> bool:
    desc = schema["$defs"]["canvas"]["properties"]["map_area"]["description"]
    return "Default when omitted: the whole canvas" in desc


def compare(dump: dict, readme: str, schema: dict) -> list[str]:
    problems: list[str] = []

    try:
        bands = parse_readme_bands(readme)
    except ValueError as e:
        return [f"README: {e}"]
    if set(bands) != set(LAYERS):
        problems.append(f"README band table lists {sorted(bands)}, expected {sorted(LAYERS)}")
    for layer in LAYERS:
        core = dump["bands"].get(layer)
        doc = bands.get(layer)
        if core is None or doc is None:
            continue
        for field in BAND_FIELDS:
            if core[field] != doc[field]:
                problems.append(
                    f"{layer}.{field}: default_band() has {core[field]}, "
                    f"schema/README.md has {doc[field]}"
                )
    if not dump["config_layers_use_default_band"]:
        problems.append("Config::layers does not start from default_band() for every layer")

    try:
        scalars = parse_readme_scalars(readme)
    except ValueError as e:
        problems.append(f"README: {e}")
        scalars = {}
    whole = [0, 0, dump["canvas_width"], dump["canvas_height"]]
    dump = dict(dump, map_area_whole_canvas=dump["map_area"] == whole)
    for key, doc in scalars.items():
        if dump[key] != doc:
            problems.append(f"{key}: core has {dump[key]!r}, schema/README.md has {doc!r}")

    found = collect_schema_defaults(schema)
    for pointer in sorted(set(found) - set(SCHEMA_DEFAULTS)):
        problems.append(f"schema default at {pointer} is not checked against core: add it here")
    for pointer, key in SCHEMA_DEFAULTS.items():
        if pointer not in found:
            problems.append(f"schema has no default at {pointer} (expected one)")
            continue
        expected = dump[key]
        if found[pointer] != expected:
            problems.append(
                f"schema default at {pointer} is {found[pointer]!r}, core has {expected!r}"
            )
    try:
        if not schema_map_area_whole_canvas(schema):
            problems.append("schema map_area description does not say it defaults to the whole canvas")
    except KeyError as e:
        problems.append(f"schema: no {e}")
    return problems


def self_test(dump: dict, readme: str, schema: dict) -> int:
    baseline = compare(dump, readme, schema)
    if baseline:
        print("self-test: the unmodified inputs already disagree:", *baseline, sep="\n  ")
        return 1

    def dump_with(path: list, value: object) -> dict:
        d = copy.deepcopy(dump)
        node = d
        for k in path[:-1]:
            node = node[k]
        node[path[-1]] = value
        return d

    def schema_with(pointer: list, value: object) -> dict:
        s = copy.deepcopy(schema)
        node = s
        for k in pointer[:-1]:
            node = node[k]
        node[pointer[-1]] = value
        return s

    def readme_with(old: str, new: str) -> str:
        if old not in readme:
            raise AssertionError(f"self-test cannot plant: {old!r} not in README")
        return readme.replace(old, new, 1)

    temp_row = _band_row(readme, "temperature")
    soil_row = _band_row(readme, "soil")
    cases = {
        "README comfort edge": (dump, readme_with(temp_row, temp_row.replace("19–24", "19–25")), schema),
        "README threshold": (dump, readme_with(temp_row, temp_row.replace("16 / 27", "16 / 28")), schema),
        "README 'none' made a number": (
            dump, readme_with(soil_row, soil_row.replace("none / 0.7", "0.05 / 0.7")), schema),
        "README band row deleted": (dump, readme_with(_band_row(readme, "air") + "\n", ""), schema),
        "README stale_after_s": (dump, readme_with("`10800` (3 h)", "`10801` (3 h)"), schema),
        "README hud max_items": (dump, readme_with("default `6`", "default `7`"), schema),
        "README map_area": (
            dump, readme_with("`{0, 0, width, height}`", "`{0, 0, 960, 800}`"), schema),
        "schema map_area": (dump, readme, schema_with(
            ["$defs", "canvas", "properties", "map_area", "description"], "Default: x 0, y 0")),
        "core map_area": (dump_with(["map_area"], [0, 0, 960, 800]), readme, schema),
        "core map_rotation": (dump_with(["map_rotation"], 90), readme, schema),
        "schema map_rotation": (dump, readme, schema_with(
            ["$defs", "canvas", "properties", "map_rotation", "default"], 270)),
        "schema stale_after_s": (dump, readme, schema_with(["properties", "stale_after_s", "default"], 10801)),
        "schema idw_power": (dump, readme, schema_with(["$defs", "heat", "properties", "idw_power", "default"], 3)),
        "schema new unchecked default": (
            dump, readme, schema_with(["$defs", "pin", "properties", "stale_after_s", "default"], 60)),
        "core band value": (dump_with(["bands", "temperature", "scale_max"], 35.0), readme, schema),
        "core optional threshold": (dump_with(["bands", "air", "warn_below"], 300.0), readme, schema),
        "core Config ignores default_band": (dump_with(["config_layers_use_default_band"], False), readme, schema),
        "core stale_after": (dump_with(["stale_after_s"], 3600), readme, schema),
        "core default layer": (dump_with(["default_layer"], "soil"), readme, schema),
    }
    failed = 0
    for name, (d, r, s) in cases.items():
        problems = compare(d, r, s)
        status = "caught" if problems else "MISSED"
        print(f"self-test: {status}: {name}" + (f" ({problems[0]})" if problems else ""))
        failed += 0 if problems else 1
    if failed:
        print(f"self-test: {failed} planted mismatch(es) went undetected")
        return 1
    print(f"self-test: all {len(cases)} planted mismatches caught")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dump", required=True, help="path to the hearthmap_dump_defaults binary")
    ap.add_argument("--readme", required=True)
    ap.add_argument("--schema", required=True)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    dump = json.loads(subprocess.run([args.dump], check=True, capture_output=True, text=True).stdout)
    with open(args.readme, encoding="utf-8") as f:
        readme = f.read()
    with open(args.schema, encoding="utf-8") as f:
        schema = json.load(f)

    if args.self_test:
        return self_test(dump, readme, schema)
    problems = compare(dump, readme, schema)
    if problems:
        print("core defaults disagree with the schema docs:", *problems, sep="\n  ")
        return 1
    print(f"core defaults match schema/README.md and the schema ({len(LAYERS)} bands, "
          f"{len(SCHEMA_DEFAULTS)} schema defaults)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
