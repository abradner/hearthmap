"""Synthetic soil-moisture series with known answers. Deterministic (seeded); no real data."""

from __future__ import annotations

import random

from hearthmap_calibrate.history import Sample

HOUR = 3600.0
DAY = 86400.0
T0 = 1_767_225_600.0  # 2026-01-01T00:00:00Z


def sawtooth(
    *,
    days: float = 28,
    period_days: float = 7,
    dry: float = 10.0,
    wet: float = 60.0,
    step_hours: float = 1.0,
    noise: float = 0.0,
    seed: int = 1,
    first_watering_day: float = 3.5,
    drain_hours: float = 0.0,
    overshoot: float = 0.0,
) -> list[Sample]:
    """Linear drying from wet to dry over each period, then an instant watering back to wet.
    With overshoot, the reading jumps past wet and drains back over drain_hours (a real pot does)."""
    rng = random.Random(seed)
    out = []
    n = int(days * 24 / step_hours)
    for k in range(n + 1):
        t = k * step_hours * HOUR
        # phase: time since the last watering (watering at first_watering_day + m * period)
        since = (t / DAY - first_watering_day) % period_days
        if t / DAY < first_watering_day:
            since = period_days - (first_watering_day - t / DAY)
        frac = since / period_days
        v = wet + (dry - wet) * frac
        if overshoot and since * 24 < drain_hours:
            v += overshoot * (1 - since * 24 / drain_hours)
        v += rng.gauss(0, noise) if noise else 0.0
        out.append(Sample(T0 + t, v))
    return out


def only_dries(*, days: float = 21, start: float = 55.0, end: float = 8.0, step_hours: float = 1.0) -> list[Sample]:
    n = int(days * 24 / step_hours)
    return [Sample(T0 + k * step_hours * HOUR, start + (end - start) * k / n) for k in range(n + 1)]


def constant(value: float, *, days: float = 21, step_hours: float = 1.0) -> list[Sample]:
    n = int(days * 24 / step_hours)
    return [Sample(T0 + k * step_hours * HOUR, value) for k in range(n + 1)]


def invert(samples: list[Sample], scale: float = 1.0, offset: float = 0.0) -> list[Sample]:
    """Mirror a series so that it rises as it dries: v' = offset - scale * v."""
    return [Sample(s.t, None if s.v is None else offset - scale * s.v) for s in samples]


def with_gaps(samples: list[Sample], every: int = 50, length: int = 6) -> list[Sample]:
    """Replace runs of `length` samples every `every` samples with unavailable."""
    return [Sample(s.t, None) if (k % every) < length and k > every else s for k, s in enumerate(samples)]


def with_outliers(samples: list[Sample], positions: list[int], value: float) -> list[Sample]:
    out = list(samples)
    for p in positions:
        out[p] = Sample(out[p].t, value)
    return out


def change_only(samples: list[Sample], resolution: float = 1.0) -> list[Sample]:
    """What Home Assistant records: rounded to `resolution`, and a sample only when it changes."""
    out, last = [], object()
    for s in samples:
        v = None if s.v is None else round(s.v / resolution) * resolution
        if v != last:
            out.append(Sample(s.t, v))
            last = v
    return out
