"""Pure Q10 temperature-acceleration math for predicting a use-by date.

No file or network I/O lives here - see data_io.py for reading/writing
CSVs and pipeline.py for wiring this math to real data. Keeping this file
free of I/O is what makes every function here trivial to unit test.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Optional

from prediction.models import Reading


def rate_factor(temp_c: float, t_ref_c: float, q10: float) -> float:
    """Spoilage rate at temp_c, relative to the rate at t_ref_c.

    k(T) = Q10 ^ ((T - T_ref) / 10). k=1 means "spoiling at the reference
    rate", k=2 means "spoiling twice as fast as at the reference rate".
    """
    return q10 ** ((temp_c - t_ref_c) / 10.0)


def derive_q10_from_ratio(rate_ratio: float, delta_t_c: float) -> float:
    """Back out Q10 from a measured rate ratio over a temperature gap.

    If spoilage at the warmer temperature happened `rate_ratio` times
    faster than at the colder temperature, and the gap between them was
    `delta_t_c` degrees, this is the Q10 implied by that single
    measurement (Taoukis et al., 1997).
    """
    return rate_ratio ** (10.0 / delta_t_c)


def pct_shelf_life_used(equivalent_hours: float, l_ref_hours: float) -> float:
    """Accumulated equivalent hours as a percentage of the reference shelf
    life; `l_ref_hours` must be > 0. Can exceed 100 (already past due)."""
    return (equivalent_hours / l_ref_hours) * 100.0


def classify_status(pct_used: float, use_soon_pct: float, past_due_pct: float) -> str:
    if pct_used >= past_due_pct:
        return "Past use-by"
    if pct_used >= use_soon_pct:
        return "Use soon"
    return "Fresh"


def filter_plausible_temperatures(
    readings: list[Reading], valid_range_c: tuple[float, float]
) -> tuple[list[Reading], int]:
    """Drop readings with a missing or out-of-sensor-range temperature.

    Returns (clean_readings, dropped_count). `valid_range_c` is a sensor
    sanity check (e.g. the MCU/BME280 couldn't plausibly report -40C or
    200C), not a food-safety bound.
    """
    lo, hi = valid_range_c
    clean: list[Reading] = []
    dropped = 0
    for r in readings:
        if r.temp_c is None or not (lo <= r.temp_c <= hi):
            dropped += 1
            continue
        clean.append(r)
    return clean, dropped


@dataclass
class Gap:
    start: datetime
    end: datetime
    actual_hours: float
    capped_hours: float


@dataclass
class AccumulationResult:
    equivalent_hours: float
    gaps: list[Gap]


def accumulate_equivalent_hours(
    readings: list[Reading], t_ref_c: float, q10: float, max_dt_hours: float
) -> AccumulationResult:
    """Sum k(T_i) * dt_i across consecutive readings.

    `readings` must already be sorted ascending by `server_ts` and
    filtered to plausible temperatures. The first reading contributes no
    time (there is no prior reading to measure a gap from). A gap longer
    than `max_dt_hours` (e.g. a Wi-Fi outage) is capped before being
    multiplied by k, and recorded so the caller can report it.

    Each interval's time is charged at the ARRIVING reading's temperature
    (trailing-edge attribution), so a transition like fridge-to-counter is
    conservatively charged at the warmer endpoint for the whole interval.
    """
    equivalent_hours = 0.0
    gaps: list[Gap] = []
    for prev, cur in zip(readings, readings[1:]):
        actual_hours = (cur.server_ts - prev.server_ts).total_seconds() / 3600.0
        dt_hours = min(actual_hours, max_dt_hours)
        if actual_hours > max_dt_hours:
            gaps.append(Gap(prev.server_ts, cur.server_ts, actual_hours, dt_hours))
        equivalent_hours += rate_factor(cur.temp_c, t_ref_c, q10) * dt_hours
    return AccumulationResult(equivalent_hours, gaps)


def forecast_temperature(
    readings: list[Reading], window_hours: float, now: datetime
) -> Optional[float]:
    """Mean temperature over the last `window_hours` up to `now`.

    Returns None if no readings fall in the window (the caller has to
    decide what to do - this module makes no policy decisions).
    """
    window_start = now - timedelta(hours=window_hours)
    in_window = [
        r.temp_c for r in readings
        if r.temp_c is not None and window_start <= r.server_ts <= now
    ]
    if not in_window:
        return None
    return sum(in_window) / len(in_window)


@dataclass
class UseByPrediction:
    remaining_hours: float
    predicted_use_by: datetime


def predict_use_by(
    equivalent_hours: float,
    l_ref_hours: float,
    forecast_temp_c: float,
    t_ref_c: float,
    q10: float,
    now: datetime,
) -> UseByPrediction:
    """Project forward from the accumulated equivalent hours.

    remaining_ref_hours is how much "equivalent shelf life at T_ref" is
    left. Dividing by k(T_forecast) converts that back into real elapsed
    hours at the forecast temperature. A negative remaining_hours means
    the food is already past its use-by at the reference rate.
    """
    remaining_ref_hours = l_ref_hours - equivalent_hours
    k_forecast = rate_factor(forecast_temp_c, t_ref_c, q10)
    remaining_hours = remaining_ref_hours / k_forecast
    predicted_use_by = now + timedelta(hours=remaining_hours)
    return UseByPrediction(remaining_hours, predicted_use_by)


def mq135_disagrees(status: str, gas_increase: Optional[float], danger_ratio: float) -> bool:
    """True when the model says Fresh but the gas sensor says otherwise.

    This only checks one direction (model optimistic, sensor pessimistic)
    because that is the safety-relevant case: it's the one where trusting
    the model alone would be a mistake. See SOURCES.md "Open questions"
    for whether the reverse case is worth adding.
    """
    if status != "Fresh" or gas_increase is None:
        return False
    return gas_increase >= danger_ratio


def count_outside_q10_range(readings: list[Reading], valid_range_c: tuple[float, float]) -> int:
    """Count readings whose temperature falls outside the range the Q10
    source data actually covers (config.Q10_VALID_TEMP_RANGE_C).

    Those readings are still used by accumulate_equivalent_hours - this
    is purely a "trust this less" counter surfaced in PredictionRow and
    printed by the CLI, not a filter. Missing temperatures are not
    counted here; filter_plausible_temperatures already handles those.
    """
    lo, hi = valid_range_c
    return sum(
        1 for r in readings
        if r.temp_c is not None and not (lo <= r.temp_c <= hi)
    )
