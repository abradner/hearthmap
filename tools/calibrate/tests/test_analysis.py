"""The method, on synthetic series with known answers."""

from __future__ import annotations

import math

import pytest

from hearthmap_calibrate.analysis import (
    INSUFFICIENT_HISTORY,
    INVERTED,
    NO_VALID_SAMPLES,
    NO_VARIATION,
    NORMAL,
    OK,
    TOO_NARROW,
    Params,
    analyse,
    weighted_percentile,
)
from hearthmap_calibrate.history import Sample
from synth import DAY, HOUR, T0, change_only, constant, invert, only_dries, sawtooth, with_gaps, with_outliers


def codes(p):
    return {w.code for w in p.warnings}


def clip0(samples):
    return [Sample(s.t, max(0.0, s.v)) for s in samples]


# --- known answers ---------------------------------------------------------------------------


def test_clean_sawtooth_recovers_range_events_and_rate():
    p = analyse("sensor.example_a", sawtooth(dry=10, wet=60, period_days=7, days=28))
    assert p.status == OK
    assert p.polarity == NORMAL
    assert p.watering_events == 4  # waterings at days 3.5, 10.5, 17.5, 24.5
    assert p.dry == pytest.approx(10, abs=1.5)  # trough: the 3 h before the rise
    assert p.wet == pytest.approx(60, abs=2.5)  # plateau: 1..13 h after, still drying a little
    assert p.drying_rate_per_day == pytest.approx(50 / 7, rel=0.02)
    assert p.drying_fraction_per_day == pytest.approx(p.drying_rate_per_day / (p.wet - p.dry), rel=1e-3)
    assert p.confidence == "high"
    assert "troughs" in p.evidence["dry_method"] and "plateaus" in p.evidence["wet_method"]
    assert p.warnings == ()


def test_noise_does_not_create_or_hide_events():
    p = analyse("sensor.example_a", sawtooth(noise=1.5, seed=7))
    assert p.status == OK and p.watering_events == 4
    assert p.dry == pytest.approx(10, abs=2.5) and p.wet == pytest.approx(60, abs=3)


def test_overshoot_and_drain_settle_to_the_plateau_not_the_peak():
    p = analyse("sensor.example_a", sawtooth(overshoot=10, drain_hours=3))
    assert p.watering_events == 4
    assert p.wet == pytest.approx(60, abs=2.5)  # not 70: the 1 h settle skips most of the drain


def test_gaps_are_ignored_and_reported():
    p = analyse("sensor.example_a", with_gaps(sawtooth(), every=50, length=6))
    assert p.status == OK
    assert "unavailable_samples" in codes(p)
    assert p.evidence["unavailable"] > 0
    assert p.watering_events >= 3  # a watering hidden inside a gap is not invented
    assert p.dry == pytest.approx(10, abs=2) and p.wet == pytest.approx(60, abs=3)


def test_isolated_outliers_are_removed_both_directions():
    s = with_outliers(sawtooth(), [30, 31], 99.0)
    s = with_outliers(s, [100], 0.0)
    p = analyse("sensor.example_a", s)
    assert p.evidence["outliers"] == 3
    assert p.watering_events == 4  # an upward spike is not a watering
    assert p.dry == pytest.approx(10, abs=1.5) and p.wet == pytest.approx(60, abs=2.5)


def test_a_spike_at_the_very_start_is_kept_because_it_could_be_a_step():
    # The series starts dry and is watered at the second sample: a step, not a spike.
    s = [Sample(T0, 0.0)] + [Sample(x.t + HOUR, x.v) for x in sawtooth(first_watering_day=0)]
    p = analyse("sensor.example_a", s, "%")
    assert p.evidence["outliers"] == 0


def test_physically_impossible_samples_are_dropped_for_percent_units():
    s = with_outliers(sawtooth(), [40, 140], 150.0)
    s = with_outliers(s, [240], -3.0)
    p = analyse("sensor.example_a", s, "%")
    assert p.evidence["impossible"] == 3 and "impossible_samples" in codes(p)
    assert p.wet == pytest.approx(60, abs=2.5)
    # Without a unit there are no implicit bounds; the despiker catches them instead.
    assert analyse("sensor.example_a", s).evidence["impossible"] == 0


def test_explicit_valid_range_applies_to_raw_units():
    s = with_outliers(invert(sawtooth(), scale=32, offset=3220), [50], 9000.0)
    p = analyse("sensor.example_raw", s, None, Params(valid_min=0, valid_max=4095))
    assert p.evidence["impossible"] == 1


# --- the cases the brief names ---------------------------------------------------------------


def test_stuck_at_zero_cannot_be_calibrated_no_variation():
    p = analyse("sensor.example_stuck", constant(0.0), "%")
    assert p.status == NO_VARIATION
    assert p.message == "cannot calibrate: no variation (every valid reading is 0)"
    assert p.dry is None and p.wet is None and p.happy_min is None and p.happy_max is None
    assert p.confidence == "none"


def test_stuck_at_zero_with_a_blip_is_still_no_variation():
    p = analyse("sensor.example_stuck", with_outliers(constant(0.0), [100], 37.0), "%")
    assert p.status == NO_VARIATION


def test_stuck_at_a_mid_value_is_no_variation_too():
    assert analyse("sensor.example_stuck", constant(42.0)).status == NO_VARIATION


def test_inverted_probe_rising_as_it_dries():
    # Raw ADC counts: 3220 - 32 * pct, so 10% -> 2900 (dry) and 60% -> 1300 (wet).
    p = analyse("sensor.example_raw", invert(sawtooth(dry=10, wet=60), scale=32, offset=3220))
    assert p.status == OK
    assert p.polarity == INVERTED
    assert p.dry > p.wet
    assert p.dry == pytest.approx(2900, abs=50)
    assert p.wet == pytest.approx(1300, abs=80)
    assert p.watering_events == 4
    assert p.drying_rate_per_day == pytest.approx(1600 / 7, rel=0.02)  # positive: drying
    assert p.wet < p.happy_min < p.dry  # happy_min 30% of the way from dry toward wet
    assert p.happy_max == p.wet


def test_inverted_probe_that_only_dries_is_detected_from_the_trend():
    p = analyse("sensor.example_raw", invert(only_dries(), scale=32, offset=3220))
    assert p.polarity == INVERTED and p.dry > p.wet
    assert "polarity_from_trend" in codes(p)


def test_sensor_that_only_dries():
    p = analyse("sensor.example_drying", only_dries(start=55, end=8, days=21))
    assert p.status == OK
    assert p.watering_events == 0
    assert p.polarity == NORMAL
    assert "no_watering_events" in codes(p) and "polarity_from_trend" in codes(p)
    assert p.confidence == "low"
    assert "percentile" in p.evidence["dry_method"] and "percentile" in p.evidence["wet_method"]
    assert p.wet == pytest.approx(55 - 47 * 0.05, abs=0.5)
    assert p.dry == pytest.approx(8 + 47 * 0.05, abs=0.5)
    assert p.drying_rate_per_day == pytest.approx(47 / 21, rel=0.02)


def test_dry_end_saturating_at_zero_is_real_and_reported():
    # The project's motivating case: a pot that sits at 0% for days before each watering.
    s = change_only(clip0(sawtooth(dry=-30, wet=40, period_days=10, days=42)))
    p = analyse("sensor.example_parched", s, "%")
    assert p.status == OK
    assert p.dry == 0.0  # 0 is a real reading, not missing data
    assert p.wet == pytest.approx(40, abs=3)
    assert "saturated_dry_end" in codes(p)
    assert p.confidence == "medium"  # saturation caps confidence below high
    assert p.evidence["flatlined"] == 0  # at an end: kept, not ignored


def test_frozen_mid_range_run_is_ignored_for_percentiles():
    s = sawtooth(days=35)
    # Freeze the sensor at 33.0 for four days in the middle (HA records it once, then nothing).
    start, stop = T0 + 12 * DAY, T0 + 16 * DAY
    frozen = [x for x in s if not (start < x.t < stop)]
    frozen = [Sample(x.t, 33.0) if x.t == start else x for x in frozen]
    p = analyse("sensor.example_a", frozen)
    assert p.evidence["flatlined"] == 1 and "flatline_ignored" in codes(p)
    assert p.confidence == "medium"


def test_change_only_recording_like_home_assistant():
    p = analyse("sensor.example_a", change_only(sawtooth(noise=0.3, seed=3)))
    assert p.status == OK and p.watering_events == 4
    assert p.dry == pytest.approx(10, abs=2) and p.wet == pytest.approx(60, abs=3)


def test_watering_recorded_hours_after_the_last_change_still_counts():
    # Sparse: the last reading before each watering came 10 h earlier (nothing changed since).
    samples = []
    for k in range(4):
        base = T0 + k * 7 * DAY
        samples += [Sample(base + h * HOUR, 60 - h * 50 / 160) for h in range(0, 160, 8)]
    p = analyse("sensor.example_a", samples)
    assert p.watering_events == 3


# --- refusals and boundaries -----------------------------------------------------------------


def test_no_valid_samples():
    p = analyse("sensor.example_a", [Sample(T0 + k * HOUR, None) for k in range(100)])
    assert p.status == NO_VALID_SAMPLES and p.message == "cannot calibrate: no valid samples"
    assert p.evidence["unavailable"] == 100 and p.dry is None


def test_empty_history_is_no_valid_samples():
    assert analyse("sensor.example_a", []).status == NO_VALID_SAMPLES


def test_too_little_history_is_refused():
    p = analyse("sensor.example_a", sawtooth(days=0.5))
    assert p.status == INSUFFICIENT_HISTORY and "too little history" in p.message


def test_too_few_samples_is_refused_even_over_days():
    p = analyse("sensor.example_a", [Sample(T0, 50.0), Sample(T0 + 5 * DAY, 20.0)])
    assert p.status == INSUFFICIENT_HISTORY


def test_short_but_usable_history_warns_and_caps_confidence():
    p = analyse("sensor.example_a", sawtooth(days=6, period_days=2, first_watering_day=1))
    assert p.status == OK and "short_history" in codes(p) and p.confidence == "low"


@pytest.mark.parametrize("days,expected", [(6.99, "low"), (7.0, "medium")])
def test_min_history_boundary(days, expected):
    # One watering only, so "high" is out of reach either way; the boundary is low vs medium.
    s = [x for x in sawtooth(days=20, period_days=10, first_watering_day=3) if x.t <= T0 + days * DAY]
    p = analyse("sensor.example_a", s)
    assert p.watering_events == 1
    assert p.confidence == expected


def test_one_watering_takes_dry_from_percentiles():
    p = analyse("sensor.example_a", sawtooth(days=10, period_days=7, first_watering_day=4))
    assert p.watering_events == 1 and "one_watering_event" in codes(p)
    assert "percentile" in p.evidence["dry_method"] and "plateau" in p.evidence["wet_method"]


def test_range_inside_the_noise_is_too_narrow():
    import random

    rng = random.Random(5)
    s = [Sample(T0 + k * HOUR, 30 + rng.gauss(0, 2)) for k in range(24 * 21)]
    p = analyse("sensor.example_noisy", s)
    assert p.status == TOO_NARROW and p.dry is None and p.wet is None


def test_forced_polarity_overrides_detection():
    s = sawtooth()
    p = analyse("sensor.example_a", s, None, Params(polarity=INVERTED))
    assert p.polarity == INVERTED  # the operator's word wins, even when it is odd


def test_happy_range_is_the_stated_fraction_of_the_range():
    p = analyse("sensor.example_a", sawtooth(), None, Params(happy_min_fraction=0.25, happy_max_fraction=0.75))
    span = p.wet - p.dry
    assert p.happy_min == pytest.approx(p.dry + 0.25 * span, abs=0.01)
    assert p.happy_max == pytest.approx(p.dry + 0.75 * span, abs=0.01)


def test_no_drying_trend_is_reported():
    # A staircase: watered twice, never seen drying in between.
    s = [Sample(T0 + h * HOUR, 10.0 if h < 48 else 50.0 if h < 108 else 80.0) for h in range(0, 168)]
    p = analyse("sensor.example_a", s)
    assert p.status == OK and p.watering_events == 2
    assert p.drying_rate_per_day is None and p.drying_fraction_per_day is None
    assert "no_drying_trend" in codes(p)


def test_weighted_percentile_boundaries():
    assert weighted_percentile([1, 2, 3], [1, 1, 1], 0) == 1
    assert weighted_percentile([1, 2, 3], [1, 1, 1], 100) == 3
    assert weighted_percentile([1, 2, 3], [1, 1, 1], 50) == 2
    assert weighted_percentile([1, 2, 3], [8, 1, 1], 50) == 1  # time-weighted, not counted
    assert weighted_percentile([1, 2, 3], [0, 0, 1], 5) == 3  # zero weight never selected
    with pytest.raises(ValueError):
        weighted_percentile([1], [0], 50)


def test_output_is_finite_everywhere():
    for s in (sawtooth(), only_dries(), invert(sawtooth(), 32, 3220)):
        p = analyse("sensor.example_a", s)
        for x in (p.dry, p.wet, p.happy_min, p.happy_max, p.drying_rate_per_day):
            assert x is None or math.isfinite(x)
