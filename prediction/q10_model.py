"""Pure Q10 temperature-acceleration math for predicting a use-by date.

No file or network I/O lives here - see data_io.py for reading/writing
CSVs and pipeline.py for wiring this math to real data. Keeping this file
free of I/O is what makes every function here trivial to unit test.
"""
from __future__ import annotations


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
    return (equivalent_hours / l_ref_hours) * 100.0


def classify_status(pct_used: float, use_soon_pct: float, past_due_pct: float) -> str:
    if pct_used >= past_due_pct:
        return "Past use-by"
    if pct_used >= use_soon_pct:
        return "Use soon"
    return "Fresh"


from dataclasses import dataclass
from datetime import datetime

from prediction.models import Reading


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
