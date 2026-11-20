"""Command line: fetch, propose, merge. Run `python3 tools/calibrate/calibrate.py --help`."""

from __future__ import annotations

import argparse
import os
import sys

from datetime import datetime, timedelta, timezone

from . import __version__
from .analysis import Params, analyse
from .document import build_document, render_report
from .files import ToolError, dumps, load_json, read_token, write_new
from .history import History, HistoryError, fetch_ha_history, history_to_json, load_history_json
from .merge import CALIBRATED_KINDS, merge_config


def _moisture_entities(config: dict) -> list[str]:
    out = []
    for pin in config.get("pins", []):
        entity = pin.get("entities", {}).get("moisture")
        if entity and pin.get("kind") in CALIBRATED_KINDS and entity not in out:
            out.append(entity)
    return out


def _entities(args) -> list[str]:
    entities = list(args.entity or [])
    if args.config:
        entities += [e for e in _moisture_entities(load_json(args.config, "config")) if e not in entities]
    if not entities:
        raise ToolError("no entities: pass --entity (repeatable) or --config")
    return entities


def _fetch(args) -> History:
    token = read_token(args.token_env, args.token_file)
    end = datetime.now(timezone.utc)
    start = end - timedelta(days=args.days)
    return fetch_ha_history(args.ha_url, token, _entities(args), start, end)


def _params(args) -> Params:
    kw = {}
    if args.valid_range:
        kw["valid_min"], kw["valid_max"] = args.valid_range
    if args.happy:
        kw["happy_min_fraction"], kw["happy_max_fraction"] = args.happy
    if args.polarity:
        kw["polarity"] = args.polarity
    if args.min_history_days is not None:
        kw["min_history_days"] = args.min_history_days
    return Params(**kw)


def cmd_fetch(args) -> int:
    history = _fetch(args)
    write_new(args.out, dumps(history_to_json(history)), force=args.force)
    print(f"wrote {args.out}: {len(history.sensors)} sensors", file=sys.stderr)
    return 0


def cmd_propose(args) -> int:
    params = _params(args)
    if not 0 <= params.happy_min_fraction <= params.happy_max_fraction <= 1:
        raise ToolError("--happy needs 0 <= LOW <= HIGH <= 1")
    history = load_history_json(args.history) if args.history else _fetch(args)
    if args.entity:  # with --history, --entity narrows the set
        missing = [e for e in args.entity if e not in history.sensors]
        if missing:
            raise ToolError(f"not in the history: {', '.join(missing)}")
        history.sensors = {e: history.sensors[e] for e in args.entity}
    proposals = [analyse(e, s, history.units.get(e), params) for e, s in history.sensors.items()]
    generated_at = args.generated_at or datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    write_new(args.out, dumps(build_document(proposals, params, generated_at)), force=args.force)
    report = render_report(proposals, params)
    if args.report:
        write_new(args.report, report, force=args.force)
    else:
        sys.stdout.write(report)
    return 0


def cmd_merge(args) -> int:
    if os.path.abspath(args.out) == os.path.abspath(args.config) or (
        os.path.exists(args.out) and os.path.samefile(args.out, args.config)
    ):
        raise ToolError("merge never edits a config in place: --out must be a different file")
    config = load_json(args.config, "config")
    doc = load_json(args.calibration, "calibration file")
    if doc.get("version") != 1 or not isinstance(doc.get("sensors"), list):
        raise ToolError(f"{args.calibration} is not a version 1 calibration file")
    merged, actions = merge_config(config, doc, replace_happy=args.replace_happy)
    write_new(args.out, dumps(merged), force=args.force)
    for a in actions:
        print(f"{a.subject}: {a.text}")
    print(f"wrote {args.out}; review it in the editor before using it", file=sys.stderr)
    return 0


def _add_ha_args(p: argparse.ArgumentParser, required: bool) -> None:
    g = p.add_argument_group("Home Assistant (the token is never a command-line argument)")
    g.add_argument("--ha-url", required=required, help="base URL, e.g. https://ha.example.test:8123")
    tok = g.add_mutually_exclusive_group()
    tok.add_argument("--token-env", metavar="NAME", help="environment variable holding the token (default HASS_TOKEN)")
    tok.add_argument("--token-file", metavar="PATH", help="file holding the token (chmod 600)")
    g.add_argument("--days", type=float, default=28.0, help="days of history to fetch (default 28)")
    g.add_argument("--config", help="take entities from this hearthmap config's soil and plant pins")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="calibrate", description="Propose per-sensor soil calibration ranges from history.")
    parser.add_argument("--version", action="version", version=f"hearthmap-calibrate {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    f = sub.add_parser("fetch", help="save Home Assistant history to a history file")
    _add_ha_args(f, required=True)
    f.add_argument("--entity", action="append", help="entity id (repeatable)")
    f.add_argument("--out", required=True, help="history file to write")
    f.add_argument("--force", action="store_true", help="overwrite an existing output")
    f.set_defaults(func=cmd_fetch)

    p = sub.add_parser("propose", help="write calibration.json and a report")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--history", help="history file (format: tools/calibrate/README.md)")
    src.add_argument("--ha-url", help="fetch from Home Assistant instead")
    tok = p.add_mutually_exclusive_group()
    tok.add_argument("--token-env", metavar="NAME", help="environment variable holding the token (default HASS_TOKEN)")
    tok.add_argument("--token-file", metavar="PATH", help="file holding the token (chmod 600)")
    p.add_argument("--days", type=float, default=28.0, help="days of history to fetch (default 28)")
    p.add_argument("--config", help="with --ha-url: take entities from this config's soil and plant pins")
    p.add_argument("--entity", action="append", help="entity id (repeatable); with --history, only these")
    p.add_argument("--out", default="calibration.json", help="interchange file (default calibration.json)")
    p.add_argument("--report", help="write the report here instead of stdout")
    p.add_argument("--happy", nargs=2, type=float, metavar=("LOW", "HIGH"), help="default happy range as fractions of dry->wet (default 0.3 1.0)")
    p.add_argument("--valid-range", nargs=2, type=float, metavar=("MIN", "MAX"), help="physically possible raw values (default 0 100 for %%, else none)")
    p.add_argument("--polarity", choices=["auto", "normal", "inverted"], help="override polarity detection (default auto)")
    p.add_argument("--min-history-days", type=float, help="days below which confidence is capped at low (default 7)")
    p.add_argument("--generated-at", help=argparse.SUPPRESS)  # fixed timestamp for reproducible output
    p.add_argument("--force", action="store_true", help="overwrite existing outputs")
    p.set_defaults(func=cmd_propose)

    m = sub.add_parser("merge", help="write a new config with the proposed ranges in its soil and plant pins")
    m.add_argument("--config", required=True, help="existing hearthmap config (read only)")
    m.add_argument("--calibration", required=True, help="calibration.json from propose")
    m.add_argument("--out", required=True, help="new config file (never the input)")
    m.add_argument("--replace-happy", action="store_true", help="replace existing happy ranges with the defaults")
    m.add_argument("--force", action="store_true", help="overwrite an existing output (never the input)")
    m.set_defaults(func=cmd_merge)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ToolError, HistoryError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


