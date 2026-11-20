"""Merge proposed ranges into a hearthmap config's soil and plant pins. Pure: dicts in, a new
dict and a list of actions out. The input config is never mutated."""

from __future__ import annotations

import copy
from dataclasses import dataclass

CALIBRATED_KINDS = ("soil", "plant")  # the only kinds that may carry calibration (schema, R1)


@dataclass(frozen=True)
class Action:
    subject: str  # a pin id, or an entity id for proposals no pin uses
    text: str


def _happy_fits(cal: dict, dry: float, wet: float) -> bool:
    """Existing happy values lie within the new [dry, wet] and happy_min is not wetter than
    happy_max (wetness measured in the range's own direction)."""
    sign = 1 if wet > dry else -1
    lo, hi = sign * dry, sign * wet
    present = [k for k in ("happy_min", "happy_max") if k in cal]
    if not all(isinstance(cal[k], (int, float)) and lo <= sign * cal[k] <= hi for k in present):
        return False
    if len(present) == 2 and sign * cal["happy_min"] > sign * cal["happy_max"]:
        return False
    return True


def merge_config(config: dict, calibration_doc: dict, *, replace_happy: bool = False) -> tuple[dict, list[Action]]:
    proposals = {s["entity_id"]: s for s in calibration_doc.get("sensors", [])}
    used: set[str] = set()
    out = copy.deepcopy(config)
    actions: list[Action] = []
    for pin in out.get("pins", []):
        entity = pin.get("entities", {}).get("moisture")
        if entity is None:
            continue
        if pin.get("kind") not in CALIBRATED_KINDS:
            actions.append(Action(pin["id"], f"skipped: kind '{pin.get('kind')}' cannot carry a calibration"))
            continue
        proposal = proposals.get(entity)
        if proposal is None:
            actions.append(Action(pin["id"], f"unchanged: no proposal for {entity}"))
            continue
        used.add(entity)
        if proposal.get("status") != "ok" or "calibration" not in proposal:
            actions.append(Action(pin["id"], f"unchanged: {proposal.get('message') or proposal.get('status')}"))
            continue
        new = proposal["calibration"]
        old = pin.get("calibration", {})
        merged = {"dry": new["dry"], "wet": new["wet"]}
        kept_happy = False
        if not replace_happy and any(k in old for k in ("happy_min", "happy_max")):
            if _happy_fits(old, new["dry"], new["wet"]):
                merged.update({k: old[k] for k in ("happy_min", "happy_max") if k in old})
                kept_happy = True
        if not kept_happy:
            merged.update({k: new[k] for k in ("happy_min", "happy_max") if k in new})
        pin["calibration"] = merged
        before = f"{old.get('dry')}..{old.get('wet')}" if old else "none"
        happy_note = "existing happy range kept" if kept_happy else "default happy range"
        if not kept_happy and not replace_happy and any(k in old for k in ("happy_min", "happy_max")):
            happy_note = "existing happy range no longer fits the new range: replaced with the default"
        actions.append(
            Action(pin["id"], f"updated: dry/wet {before} -> {new['dry']}..{new['wet']} (confidence {proposal.get('confidence')}; {happy_note})")
        )
    for entity in proposals:
        if entity not in used:
            actions.append(Action(entity, "not merged: no soil or plant pin binds this entity as moisture"))
    return out, actions
