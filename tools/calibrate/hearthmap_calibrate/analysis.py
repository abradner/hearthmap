"""Propose one soil sensor's calibration range from its history.

Pure: samples and parameters in, a Proposal out. No clock, no I/O.

Pipeline (each step is a small function below; tools/calibrate/README.md explains the method):
  1. clean      drop unavailable and physically impossible samples;
  2. despike    drop isolated outliers (a Hampel filter: far from the median of their neighbours);
  3. refuse     no valid samples, no variation at all (a stuck sensor), or too little history;
  4. holds      Home Assistant records a state only when it changes, so a sample's value holds
                until the next sample (capped at max_hold_hours); every later step works on
                these held values, which weights each value by how long it was true;
  5. flat-line  a run of one identical value longer than flatline_hours that is not at either
                end of the sensor's range is a frozen sensor and is ignored; a long run at an
                end is kept (a probe saturating at "bone dry" is real) and reported;
  6. polarity   watering is fast and drying is slow, so the direction of sharp moves is "wetter"
                (inverted probes read lower when wet); with no sharp moves, the slow trend is
                "drier";
  7. events     a watering event is a rise in the wet direction of at least min_rise within
                event_window_hours; its trough is the held value just before, its plateau the
                held value after the water has settled;
  8. range      wet = median plateau (>= 1 event), dry = median trough (>= 2 events), else
                time-weighted percentiles of the held values;
  9. rate       drying rate = median least-squares slope between events, in raw units per day;
 10. judge      refuse a range inside the noise; grade confidence; add the default happy range.
"""

from __future__ import annotations

import bisect
import math
import statistics
from dataclasses import dataclass, field

from .history import Sample

HOUR = 3600.0
DAY = 86400.0

# Statuses. Only OK carries a calibration.
OK = "ok"
NO_VALID_SAMPLES = "no_valid_samples"
NO_VARIATION = "no_variation"
INSUFFICIENT_HISTORY = "insufficient_history"
TOO_NARROW = "too_narrow"

NORMAL = "normal"  # higher reading = wetter
INVERTED = "inverted"  # higher reading = drier


@dataclass(frozen=True)
class Params:
    """Tuning knobs. Every default is documented in tools/calibrate/README.md."""

    valid_min: float | None = None  # physical bounds; None: from the unit ("%": 0..100), else none
    valid_max: float | None = None
    min_history_days: float = 7.0  # below this: a warning and confidence capped at low
    hard_min_history_days: float = 1.0  # below this: cannot calibrate
    hard_min_samples: int = 3
    despike_half_window: int = 3
    despike_k: float = 5.0
    despike_floor_fraction: float = 0.1  # of the sensor's span: deviations below this are kept
    max_hold_hours: float = 168.0  # a value holds until the next sample, at most this long
    flatline_hours: float = 72.0
    event_window_hours: float = 3.0
    min_rise_fraction: float = 0.2  # of the span: the smallest rise that counts as watering
    settle_hours: float = 1.0
    plateau_hours: float = 12.0
    trough_hours: float = 3.0
    grid_minutes: float = 30.0
    dry_percentile: float = 5.0  # on the dry side, measured in wetness (polarity-aware)
    wet_percentile: float = 95.0
    happy_min_fraction: float = 0.3  # of the way from dry to wet
    happy_max_fraction: float = 1.0
    polarity: str = "auto"  # auto | normal | inverted


@dataclass(frozen=True)
class Finding:
    code: str
    message: str


@dataclass(frozen=True)
class Proposal:
    entity_id: str
    status: str
    message: str | None = None
    dry: float | None = None
    wet: float | None = None
    happy_min: float | None = None
    happy_max: float | None = None
    confidence: str = "none"
    polarity: str | None = None
    watering_events: int = 0
    drying_rate_per_day: float | None = None
    drying_fraction_per_day: float | None = None
    evidence: dict = field(default_factory=dict)
    warnings: tuple[Finding, ...] = ()


# --- small numeric helpers --------------------------------------------------------------------


def weighted_percentile(values: list[float], weights: list[float], q: float) -> float:
    """Lower weighted percentile: the smallest value whose cumulative weight reaches q%."""
    pairs = sorted((v, w) for v, w in zip(values, weights) if w > 0)
    if not pairs:
        raise ValueError("no weighted values")
    total = sum(w for _, w in pairs)
    target = total * min(max(q, 0.0), 100.0) / 100.0
    cumulative = 0.0
    for v, w in pairs:
        cumulative += w
        if cumulative >= target - 1e-12 * total:
            return v
    return pairs[-1][0]


def slope_per_day(points: list[tuple[float, float]]) -> float | None:
    if len(points) < 3:
        return None
    mt = statistics.fmean(t for t, _ in points)
    mv = statistics.fmean(v for _, v in points)
    sxx = sum((t - mt) ** 2 for t, _ in points)
    if sxx == 0:
        return None
    return sum((t - mt) * (v - mv) for t, v in points) / sxx * DAY


def decimals_for(scale: float) -> int:
    """Decimal places that keep about four significant digits of `scale` (a span or a width),
    and never fewer than two: a fixed precision would round a sensor with tiny raw units to 0."""
    if not math.isfinite(scale) or scale <= 0:
        return 2
    return max(2, 3 - math.floor(math.log10(scale)))


def _round(x: float, decimals: int = 2) -> float:
    return round(x, decimals) + 0.0  # + 0.0 turns -0.0 into 0.0


def rounded_range(dry: float, wet: float, decimals: int) -> tuple[float, float] | None:
    """dry and wet as exported, or None when rounding would collapse or flip the range. With
    decimals_for(width) this cannot happen; the check keeps a zero-width calibration impossible
    by construction rather than by that arithmetic."""
    r_dry, r_wet = _round(dry, decimals), _round(wet, decimals)
    if (r_wet - r_dry) * (wet - dry) <= 0:
        return None
    return r_dry, r_wet


# --- pipeline steps ---------------------------------------------------------------------------


@dataclass
class _Held:
    """Valid samples with their hold ends: value v[i] is true over [t[i], end[i]). nxt[i] is the
    time of the next sample of any kind (valid or a gap): the latest moment v[i] was still the
    last word, which is where a rise that follows it can have started at the earliest."""

    t: list[float]
    v: list[float]
    end: list[float]
    nxt: list[float]
    flat: list[bool]

    def value_at(self, when: float) -> float | None:
        i = bisect.bisect_right(self.t, when) - 1
        if i < 0 or when >= self.end[i]:
            return None
        return self.v[i]

    def values_between(self, t0: float, t1: float, step: float) -> list[tuple[float, float]]:
        out = []
        when = t0
        while when <= t1:
            v = self.value_at(when)
            if v is not None:
                out.append((when, v))
            when += step
        return out


def bounds_for(unit: str | None, params: Params) -> tuple[float | None, float | None]:
    lo, hi = params.valid_min, params.valid_max
    if unit == "%":
        lo = 0.0 if lo is None else lo
        hi = 100.0 if hi is None else hi
    return lo, hi


def clean(samples: list[Sample], lo: float | None, hi: float | None) -> tuple[list[Sample], dict]:
    """Mark unavailable and impossible samples as gaps (v=None), keeping their timestamps:
    a gap ends the previous value's hold."""
    out, unavailable, impossible = [], 0, 0
    for s in sorted(samples, key=lambda s: s.t):
        if s.v is None or not math.isfinite(s.v):
            unavailable += 1
            out.append(Sample(s.t, None))
        elif (lo is not None and s.v < lo) or (hi is not None and s.v > hi):
            impossible += 1
            out.append(Sample(s.t, None))
        else:
            out.append(s)
    return out, {"samples": len(samples), "unavailable": unavailable, "impossible": impossible}


def despike(seq: list[Sample], params: Params) -> tuple[list[Sample], int]:
    """Drop isolated outliers: a run of at most despike_half_window valid samples that are each
    far from the median of their neighbours (a Hampel filter) and that the series leaves and
    comes back from, i.e. the valid samples just before and just after the run both lie on the
    same side of every value in it. A step (a watering) never comes back, and a run at either end
    of the series has no neighbour on one side, so neither is ever dropped. Dropped samples vanish
    (the previous value holds through them); gaps are untouched."""
    idx = [i for i, s in enumerate(seq) if s.v is not None]
    vals = [seq[i].v for i in idx]
    if len(vals) < 3:
        return seq, 0
    floor = params.despike_floor_fraction * (max(vals) - min(vals))
    h = params.despike_half_window
    flagged = []
    for n, v in enumerate(vals):
        window = vals[max(0, n - h) : n + h + 1]
        med = statistics.median(window)
        mad = statistics.median(abs(x - med) for x in window)
        flagged.append(abs(v - med) > max(params.despike_k * 1.4826 * mad, floor))
    drop: set[int] = set()
    n = 0
    while n < len(vals):
        if not flagged[n]:
            n += 1
            continue
        m = n
        while m + 1 < len(vals) and flagged[m + 1]:
            m += 1
        if m - n + 1 <= h and n > 0 and m + 1 < len(vals):
            run, before, after = vals[n : m + 1], vals[n - 1], vals[m + 1]
            if min(run) > max(before, after) or max(run) < min(before, after):
                drop.update(idx[k] for k in range(n, m + 1))
        n = m + 1
    return [s for i, s in enumerate(seq) if i not in drop], len(drop)


def holds(seq: list[Sample], params: Params) -> _Held:
    max_hold = params.max_hold_hours * HOUR
    valid_t = [s.t for s in seq if s.v is not None]
    gaps = [b - a for a, b in zip(valid_t, valid_t[1:])]
    last_hold = min(max_hold, statistics.median(gaps)) if gaps else 0.0
    t, v, end, nxts = [], [], [], []
    for i, s in enumerate(seq):
        if s.v is None:
            continue
        nxt = seq[i + 1].t if i + 1 < len(seq) else s.t + last_hold
        t.append(s.t)
        v.append(s.v)
        end.append(min(nxt, s.t + max_hold))
        nxts.append(nxt)
    return _Held(t, v, end, nxts, [False] * len(t))


def mark_flatlines(held: _Held, params: Params) -> tuple[int, list[float]]:
    """Runs of one value with no break, longer than flatline_hours. Mid-range runs are frozen
    sensors: marked flat (ignored) and counted. Runs at the min or max are saturation: kept,
    and their values returned so the caller can report them."""
    lo, hi = min(held.v), max(held.v)
    limit = params.flatline_hours * HOUR
    ignored, saturated = 0, set()
    i, n = 0, len(held.t)
    while i < n:
        j = i
        while j + 1 < n and held.v[j + 1] == held.v[i] and held.end[j] >= held.t[j + 1]:
            j += 1
        if held.end[j] - held.t[i] > limit:
            if held.v[i] in (lo, hi):
                saturated.add(held.v[i])
            else:
                for k in range(i, j + 1):
                    held.flat[k] = True
                ignored += j - i + 1
        i = j + 1
    return ignored, sorted(saturated)


def noise_sigma(values: list[float]) -> float:
    diffs = [b - a for a, b in zip(values, values[1:])]
    if len(diffs) < 2:
        return 0.0
    med = statistics.median(diffs)
    return 1.4826 * statistics.median(abs(d - med) for d in diffs) / math.sqrt(2)


@dataclass(frozen=True)
class Event:
    trough_i: int
    peak_i: int


def find_events(held: _Held, sign: int, min_rise: float, params: Params) -> list[Event]:
    """Rises in wetness (sign * value) of at least min_rise within event_window_hours.

    Home Assistant records only changes, so a value may have been recorded hours before the
    watering that ends it. A rise from sample i to sample j is therefore timed from nxt[i] (the
    moment i stopped being the latest sample), not from t[i]. A gap (unavailable) right after i
    is a sample too, so a rise across a long outage does not count as watering."""
    window = params.event_window_hours * HOUR
    t, w, nxt_t = held.t, [sign * x for x in held.v], held.nxt
    events: list[Event] = []
    i, n = 0, len(t)
    while i < n:
        best_j, best = None, 0.0
        j = i + 1
        while j < n and t[j] - nxt_t[i] <= window:
            if w[j] - w[i] > best:
                best_j, best = j, w[j] - w[i]
            j += 1
        if best_j is None or best < min_rise:
            i += 1
            continue
        peak = best_j
        while True:  # the rise may continue past the first window (watering in stages)
            k, nxt = peak + 1, None
            while k < n and t[k] - nxt_t[peak] <= window:
                if w[k] > w[peak] and (nxt is None or w[k] > w[nxt]):
                    nxt = k
                k += 1
            if nxt is None:
                break
            peak = nxt
        trough = min(range(i, peak), key=lambda m: (w[m], -m))
        events.append(Event(trough, peak))
        i = peak + 1
    return events


def detect_polarity(held: _Held, min_rise: float, params: Params) -> tuple[str, list[Finding]]:
    """Normal when sharp rises outnumber sharp falls, inverted when the reverse; on a tie (none of
    either, or as many of each) the slow trend decides."""
    if params.polarity in (NORMAL, INVERTED):
        return params.polarity, []
    up = len(find_events(held, +1, min_rise, params))
    down = len(find_events(held, -1, min_rise, params))
    if up != down:
        return (NORMAL if up > down else INVERTED), []
    why = "no sharp rises or falls" if up == 0 else f"as many sharp rises as falls ({up})"
    trend = trend_sign(held)
    if trend == 0:
        return NORMAL, [Finding("polarity_assumed", f"{why} and no drying trend: assumed higher reading = wetter")]
    polarity = NORMAL if trend < 0 else INVERTED
    return polarity, [Finding("polarity_from_trend", f"{why}: polarity ({polarity}) inferred from the drying trend")]


def trend_sign(held: _Held, step_hours: float = 6.0) -> int:
    """Which way the reading mostly moves: -1 falling, +1 rising, 0 neither. Counts the signs of
    changes between held values step_hours apart, so a few sharp waterings cannot outvote days of
    slow drying (a least-squares slope over a sawtooth can point either way)."""
    grid = [v for t, v in held.values_between(held.t[0], held.end[-1], step_hours * HOUR)]
    flat = {held.v[i] for i, f in enumerate(held.flat) if f}
    diffs = [b - a for a, b in zip(grid, grid[1:]) if a not in flat and b not in flat]
    up, down = sum(d > 0 for d in diffs), sum(d < 0 for d in diffs)
    return (up > down) - (up < down)


def analyse(entity_id: str, samples: list[Sample], unit: str | None = None, params: Params = Params()) -> Proposal:
    lo, hi = bounds_for(unit, params)
    seq, counts = clean(samples, lo, hi)
    seq, outliers = despike(seq, params)
    evidence = dict(counts, outliers=outliers)
    findings: list[Finding] = []
    if counts["unavailable"]:
        findings.append(Finding("unavailable_samples", f"{counts['unavailable']} unavailable samples ignored"))
    if counts["impossible"]:
        findings.append(Finding("impossible_samples", f"{counts['impossible']} samples outside {lo:g}..{hi:g} ignored as physically impossible"))
    if outliers:
        findings.append(Finding("outliers_removed", f"{outliers} isolated outliers removed"))

    valid = [s for s in seq if s.v is not None]
    evidence["valid_samples"] = len(valid)
    if not valid:
        return Proposal(entity_id, NO_VALID_SAMPLES, "cannot calibrate: no valid samples", evidence=evidence, warnings=tuple(findings))
    vmin, vmax = min(s.v for s in valid), max(s.v for s in valid)
    days = (valid[-1].t - valid[0].t) / DAY
    ev_dp = decimals_for(vmax - vmin)
    evidence.update(min=_round(vmin, ev_dp), max=_round(vmax, ev_dp), history_days=round(days, 2))
    if vmax - vmin <= 1e-9:
        return Proposal(
            entity_id, NO_VARIATION, f"cannot calibrate: no variation (every valid reading is {vmin:g})", evidence=evidence, warnings=tuple(findings)
        )
    if len(valid) < params.hard_min_samples or days < params.hard_min_history_days:
        return Proposal(
            entity_id,
            INSUFFICIENT_HISTORY,
            f"cannot calibrate: too little history ({len(valid)} valid samples over {days:.1f} days)",
            evidence=evidence,
            warnings=tuple(findings),
        )
    if days < params.min_history_days:
        findings.append(Finding("short_history", f"only {days:.1f} days of history (want {params.min_history_days:g}+)"))

    held = holds(seq, params)
    flat_count, saturated = mark_flatlines(held, params)
    evidence["flatlined"] = flat_count
    if flat_count:
        findings.append(
            Finding("flatline_ignored", f"{flat_count} samples ignored: unchanged for over {params.flatline_hours:g} h mid-range (a frozen sensor?)")
        )

    sigma = noise_sigma(held.v)
    span = vmax - vmin
    min_rise = max(params.min_rise_fraction * span, 4 * sigma)
    polarity, pol_findings = detect_polarity(held, min_rise, params)
    findings += pol_findings
    sign = 1 if polarity == NORMAL else -1
    events = find_events(held, sign, min_rise, params)

    step = params.grid_minutes * 60
    troughs, plateaus = [], []
    for k, ev in enumerate(events):
        prev_peak_t = held.t[events[k - 1].peak_i] if k else -math.inf
        next_trough_t = held.t[events[k + 1].trough_i] if k + 1 < len(events) else math.inf
        t_peak = held.t[ev.peak_i]
        # The trough value holds until the rise begins: look at the trough_hours before that.
        t_rise = held.end[ev.trough_i] - 1.0
        before = [v for _, v in held.values_between(max(t_rise - params.trough_hours * HOUR, prev_peak_t), t_rise, step)]
        troughs.append(statistics.median(before) if before else held.v[ev.trough_i])
        p0 = t_peak + params.settle_hours * HOUR
        p1 = min(p0 + params.plateau_hours * HOUR, next_trough_t - step)
        after = [v for _, v in held.values_between(p0, p1, step)] if p1 >= p0 else []
        plateaus.append(statistics.median(after) if after else held.v[ev.peak_i])

    weights = [0.0 if f else e - t for t, e, f in zip(held.t, held.end, held.flat)]
    wetness = [sign * v for v in held.v]
    if sum(weights) <= 0:
        weights = [e - t for t, e in zip(held.t, held.end)]
    pct_wet = sign * weighted_percentile(wetness, weights, params.wet_percentile)
    pct_dry = sign * weighted_percentile(wetness, weights, params.dry_percentile)
    # Events are evidence, percentiles a bound: each end is the more extreme of the two. Top-up
    # waterings of a still-moist pot have troughs far wetter than the pot's dry spells, and a
    # partial watering has a plateau short of a thorough one; neither may narrow the range.
    wet, wet_method = pct_wet, f"{params.wet_percentile:g}th wetness percentile"
    if plateaus:
        if sign * statistics.median(plateaus) >= sign * pct_wet:
            wet, wet_method = statistics.median(plateaus), f"median of {len(plateaus)} post-watering plateaus"
        else:
            wet_method += f" (wetter than the median of {len(plateaus)} post-watering plateaus)"
    dry, dry_method = pct_dry, f"{params.dry_percentile:g}th wetness percentile"
    if len(troughs) >= 2:
        if sign * statistics.median(troughs) <= sign * pct_dry:
            dry, dry_method = statistics.median(troughs), f"median of {len(troughs)} pre-watering troughs"
        else:
            dry_method += f" (drier than the median of {len(troughs)} pre-watering troughs)"
    evidence.update(dry_method=dry_method, wet_method=wet_method, percentile_dry=_round(pct_dry, ev_dp), percentile_wet=_round(pct_wet, ev_dp))

    rate = drying_rate(held, events, sign, params)
    common = dict(polarity=polarity, watering_events=len(events), evidence=evidence)
    width = sign * (wet - dry)
    if width <= max(4 * sigma, 1e-9):
        return Proposal(
            entity_id,
            TOO_NARROW,
            f"cannot calibrate: dry {dry:g} and wet {wet:g} are not separated by more than the sensor's noise",
            warnings=tuple(findings),
            **common,
        )

    if not events:
        findings.append(Finding("no_watering_events", "no watering events seen: wet is the wettest reading seen, not a post-watering reading; water thoroughly and re-run"))
    elif len(events) == 1:
        findings.append(Finding("one_watering_event", "one watering event: dry comes from percentiles, not troughs"))
    for value in saturated:
        end = "wet" if sign * value == max(wetness) else "dry"
        findings.append(
            Finding(f"saturated_{end}_end", f"reads {value:g} unchanged for over {params.flatline_hours:g} h at its {end} end: the probe may saturate there, so the true {end} end may lie beyond it")
        )
    if rate is None:
        findings.append(Finding("no_drying_trend", "no drying trend seen between waterings"))

    dp = decimals_for(width)
    exported = rounded_range(dry, wet, dp)
    if exported is None:
        return Proposal(
            entity_id,
            TOO_NARROW,
            f"cannot calibrate: dry {dry:g} and wet {wet:g} round to a zero-width range",
            warnings=tuple(findings),
            **common,
        )
    happy_min = dry + params.happy_min_fraction * (wet - dry)
    happy_max = dry + params.happy_max_fraction * (wet - dry)
    fraction = rate / abs(wet - dry) if rate is not None else None
    rate_out = _round(rate, decimals_for(rate)) if rate is not None else None
    return Proposal(
        entity_id,
        OK,
        dry=exported[0],
        wet=exported[1],
        happy_min=_round(happy_min, dp),
        happy_max=_round(happy_max, dp),
        confidence=confidence(len(events), days, params, findings),
        drying_rate_per_day=rate_out,
        drying_fraction_per_day=_round(fraction, decimals_for(fraction)) if fraction is not None else None,
        warnings=tuple(findings),
        **common,
    )


def drying_rate(held: _Held, events: list[Event], sign: int, params: Params) -> float | None:
    """Median drying slope (raw units per day, positive = drying) over the stretches between
    waterings, each at least 12 h long, sampled on the held-value grid; flat-lined runs excluded."""
    step = params.grid_minutes * 60
    starts = [held.t[0]] + [held.t[e.peak_i] + params.settle_hours * HOUR for e in events]
    ends = [held.t[e.trough_i] for e in events] + [held.end[-1]]
    flat_spans = [(t, e) for t, e, f in zip(held.t, held.end, held.flat) if f]
    slopes = []
    for a, b in zip(starts, ends):
        if b - a < 12 * HOUR:
            continue
        points = [(t, v) for t, v in held.values_between(a, b, step) if not any(s <= t < e for s, e in flat_spans)]
        s = slope_per_day(points)
        if s is not None:
            slopes.append(-sign * s)
    if not slopes:
        return None
    rate = statistics.median(slopes)
    return rate if rate > 0 else None


def confidence(events: int, days: float, params: Params, findings: list[Finding]) -> str:
    codes = {f.code for f in findings}
    if events == 0 or days < params.min_history_days:
        return "low"
    if events >= 3 and days >= 2 * params.min_history_days and not codes & {"saturated_dry_end", "saturated_wet_end", "flatline_ignored"}:
        return "high"
    return "medium"
