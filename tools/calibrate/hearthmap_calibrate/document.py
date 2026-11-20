"""The calibration.json interchange document (schema/calibration.schema.json) and the
human-readable report. Pure: proposals in, a dict or a string out."""

from __future__ import annotations

from dataclasses import asdict

from . import __version__
from .analysis import NORMAL, OK, Params, Proposal

FORMAT_VERSION = 1
SCHEMA_URL = "https://raw.githubusercontent.com/abradner/hearthmap/main/schema/calibration.schema.json"


def sensor_entry(p: Proposal) -> dict:
    entry: dict = {"entity_id": p.entity_id, "status": p.status}
    if p.status == OK:
        entry["calibration"] = {"dry": p.dry, "wet": p.wet, "happy_min": p.happy_min, "happy_max": p.happy_max}
        entry["happy_is_default"] = True
    else:
        entry["message"] = p.message
    entry["confidence"] = p.confidence
    entry["polarity"] = p.polarity
    entry["watering_events"] = p.watering_events
    entry["drying_rate_per_day"] = p.drying_rate_per_day
    entry["drying_fraction_per_day"] = p.drying_fraction_per_day
    entry["warnings"] = [{"code": w.code, "message": w.message} for w in p.warnings]
    entry["evidence"] = dict(p.evidence)
    return entry


def build_document(proposals: list[Proposal], params: Params, generated_at: str) -> dict:
    return {
        "$schema": SCHEMA_URL,
        "version": FORMAT_VERSION,
        "generated_at": generated_at,
        "generator": f"hearthmap-calibrate {__version__}",
        "parameters": asdict(params),
        "sensors": [sensor_entry(p) for p in proposals],
    }


def _num(x: float | None) -> str:
    return "n/a" if x is None else f"{x:g}"


def render_report(proposals: list[Proposal], params: Params) -> str:
    ok = sum(p.status == OK for p in proposals)
    lines = [
        "hearthmap calibration report",
        f"{len(proposals)} sensors: {ok} calibrated, {len(proposals) - ok} cannot be calibrated",
        "",
        f"happy range is a DEFAULT: {params.happy_min_fraction:.0%} to {params.happy_max_fraction:.0%} of the way from dry to wet."
        " Tune it per species in the editor.",
        "",
    ]
    for p in proposals:
        if p.status != OK:
            lines.append(f"{p.entity_id}")
            lines.append(f"  {p.message}")
        else:
            direction = "higher is wetter" if p.polarity == NORMAL else "inverted probe: lower is wetter"
            lines.append(f"{p.entity_id}  (confidence: {p.confidence})")
            lines.append(f"  range:     dry {_num(p.dry)}  ->  wet {_num(p.wet)}  ({direction})")
            lines.append(f"  happy:     {_num(p.happy_min)} .. {_num(p.happy_max)}  (default, tune per species)")
            lines.append(f"  waterings: {p.watering_events}")
            if p.drying_rate_per_day is not None and p.drying_fraction_per_day:
                lines.append(
                    f"  drying:    {_num(p.drying_rate_per_day)} units/day"
                    f" ({p.drying_fraction_per_day:.0%} of the range per day, about {1 / p.drying_fraction_per_day:.1f} days from wet to dry)"
                )
            else:
                lines.append("  drying:    unknown")
            ev = p.evidence
            lines.append(f"  method:    dry from {ev.get('dry_method')}; wet from {ev.get('wet_method')}")
        ev = p.evidence
        lines.append(
            f"  data:      {ev.get('history_days', 0):g} days, {ev.get('samples', 0)} samples"
            f" ({ev.get('unavailable', 0)} unavailable, {ev.get('impossible', 0)} impossible,"
            f" {ev.get('outliers', 0)} outliers, {ev.get('flatlined', 0)} flat-lined)"
        )
        for w in p.warnings:
            lines.append(f"  warning:   {w.message}")
        lines.append("")
    return "\n".join(lines)
