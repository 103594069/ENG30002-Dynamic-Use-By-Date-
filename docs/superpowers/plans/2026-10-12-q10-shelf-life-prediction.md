# Q10 Shelf-Life Prediction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a scheduled Python script that reads logged sensor readings (mocked in
the Sheet's HTTPS-payload schema, since the real Sheet doesn't exist yet) and computes a
predicted use-by date for raw chicken using a literature-derived Q10 temperature model,
with an MQ-135 secondary indicator and full parameter traceability.

**Architecture:** A pure-math core (`q10_model.py`) that knows nothing about files or
CSVs, fed by an I/O layer (`data_io.py`) that knows two input schemas and one output
schema, wired together by an orchestration layer (`pipeline.py`) that is the only module
allowed to read `config.py`. A thin `cli.py` makes it schedulable via cron/Task
Scheduler. This mirrors the separation the spec requires (pure functions with unit
tests, kept apart from I/O) and the pattern the repo's existing
`read-data-from-google-sheet` script already uses (stdlib-only Python, public CSV
export, no external dependencies in production code).

**Tech Stack:** Python 3.10+, stdlib only (`csv`, `argparse`, `dataclasses`, `datetime`)
for production code; `pytest` as the only dev dependency. No `gspread`, no `pandas` —
matches the existing `read-data-from-google-sheet` script's conventions and keeps every
line explainable.

**Spec:** `docs/superpowers/specs/2026-10-12-q10-shelf-life-prediction-spec.md`

## Global Constraints

- Pure Q10 math lives in `q10_model.py` only; no file or network I/O there. Every
  function takes its parameters as explicit arguments (no reading `config` inside
  `q10_model.py`) so it is testable with zero setup.
- Nothing is hardcoded outside `config.py`. Every tunable value carries a comment with
  its source and justification.
- CSVs are parsed by header name, never column position (both `load_https_payload_csv`
  and `load_sd_datacsv`).
- `dt` between readings comes from real timestamp gaps, capped at `MAX_DT_HOURS`
  (default 2h); every capped gap is counted and reported.
- Temperature readings that are missing or outside `PLAUSIBLE_TEMP_RANGE_C` are dropped
  before accumulation, and the drop count is reported.
- Every prediction reports a central estimate plus an earliest/latest use-by from the
  Q10 sensitivity band (1.5 / 2.0), and a Fresh / Use soon / Past use-by status from
  configurable thresholds.
- MQ-135 (`gas_increase`) is reported alongside every prediction as an independent
  indicator and is never an input to the Q10 maths. A disagreement (model says Fresh,
  sensor says danger) is flagged.
- Every output row records `T_ref, L_ref, Q10, Q10 band, forecast window, config
  version` so any prediction is traceable to the parameters that produced it.
- The real Apps Script / Google Sheet is unverified (`PIPELINE_PLAN.md`, open risk #1).
  This plan treats that as a standing assumption, not something it resolves — all
  development and testing here uses mock CSV data in the HTTPS-payload schema.
- `config.Q10 = 1.75` does not match what the stored Mumbre inputs compute to (≈1.69).
  This is never silently resolved: `pipeline.q10_discrepancy_message()` prints both
  numbers every time the CLI runs, until a human changes `config.py`.
- Citations in `SOURCES.md` are IEEE style, real, and verifiable. The FoodSafety.gov
  primary-source URL is deliberately left for the user to fill in (see Task 10) rather
  than guessed.

## File Structure

```
prediction/
  __init__.py             # empty, makes this a package
  models.py                # Reading, PredictionRow dataclasses (no logic, no I/O)
  config.py                # every tunable parameter, with source/justification comments
  q10_model.py              # pure Q10 math: rate_factor, accumulation, prediction, status
  data_io.py                # CSV readers (two schemas) + predictions CSV writer
  pipeline.py                # wires config.py + data_io.py + q10_model.py together
  cli.py                     # argparse entry point, schedulable via cron/Task Scheduler
  mock_data/
    mock_sheet.csv            # synthetic HTTPS-payload-schema trial data for dev/tests
  tests/
    __init__.py
    test_q10_model.py
    test_data_io.py
    test_pipeline.py
    test_cli.py
  README.md                   # usage, scheduling, assumptions, limitations
  SOURCES.md                  # IEEE-style citation log + Q10 derivation + open questions
```

- `models.py` has no dependencies, so every other module can import it without cycles.
- `q10_model.py` depends only on `models.py` — this is what makes it unit-testable with
  hand-built `Reading` objects and no files.
- `data_io.py` depends only on `models.py` — it never imports `config.py` or
  `q10_model.py`, so schema parsing can be tested in isolation from the math.
- `pipeline.py` is the one place `config.py`, `data_io.py`, and `q10_model.py` meet.
- `cli.py` depends only on `pipeline.py`, `data_io.py`, and `config.py`.

## Task 1: Scaffolding, data models, and config

**Files:**
- Create: `prediction/__init__.py`
- Create: `prediction/models.py`
- Create: `prediction/config.py`
- Create: `prediction/tests/__init__.py`

**Interfaces:**
- Produces: `Reading` dataclass (`server_ts, device_ts, trial, source, temp_c,
  humidity_pct, pressure_hpa, lux, mq135_raw, gas_base, gas_increase, risk` — all
  `Optional` except `server_ts`), `PredictionRow` dataclass (18 fields, including
  `q10_range_exceeded_count` — see Task 8 for how it's populated), and every
  `config.py` constant used by later tasks:
  `T_REF_C, L_REF_HOURS, Q10_LITERATURE_RATE_RATIO, Q10_LITERATURE_DELTA_T_C, Q10,
  Q10_BAND, Q10_VALID_TEMP_RANGE_C, FORECAST_WINDOW_HOURS, MAX_DT_HOURS,
  PLAUSIBLE_TEMP_RANGE_C, FOOD_START_TS, MQ135_DANGER_RATIO, STATUS_USE_SOON_PCT,
  STATUS_PAST_DUE_PCT, CONFIG_VERSION`.

- [ ] **Step 1: Create the package and empty test package**

```bash
mkdir -p prediction/tests prediction/mock_data
touch prediction/__init__.py prediction/tests/__init__.py
```

- [ ] **Step 2: Write `prediction/models.py`**

```python
"""Shared data shapes used across the prediction pipeline.

Kept dependency-free (no I/O, no Q10 math) so every other module can
import it without creating import cycles.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Optional


@dataclass
class Reading:
    """One sensor sample, normalised from whichever CSV schema it came from."""
    server_ts: datetime
    device_ts: Optional[datetime]
    trial: Optional[str]
    source: Optional[str]
    temp_c: Optional[float]
    humidity_pct: Optional[float]
    pressure_hpa: Optional[float]
    lux: Optional[float]
    mq135_raw: Optional[float]
    gas_base: Optional[float]
    gas_increase: Optional[float]
    risk: Optional[str]


@dataclass
class PredictionRow:
    """One row of pipeline output, written to predictions.csv."""
    run_ts: datetime
    equivalent_hours: float
    pct_shelf_life_used: float
    remaining_hours: float
    predicted_use_by: datetime
    predicted_use_by_earliest: datetime
    predicted_use_by_latest: datetime
    status: str
    mq135_gas_increase: Optional[float]
    mq135_disagrees: bool
    gap_count: int
    dropped_reading_count: int
    q10_range_exceeded_count: int
    t_ref_c: float
    l_ref_hours: float
    q10: float
    q10_band_low: float
    q10_band_high: float
    forecast_window_hours: float
    config_version: str
```

- [ ] **Step 3: Write `prediction/config.py`**

```python
"""All tunable parameters for the use-by prediction model.

Nothing in q10_model.py, data_io.py, or pipeline.py is hardcoded - every
number that affects a prediction lives here, with its source and
justification as a comment. See SOURCES.md for full citations and
README.md "Limitations" for what each assumption costs us.
"""
from datetime import datetime
from typing import Optional

# --- Q10 model reference point -------------------------------------------

# Source: Mumbre et al. (refrigerated condition) and US FoodSafety.gov
# chicken guidance (<=4C / 40F). Chosen so the model's reference
# temperature matches both the borrowed Q10 dataset and the food-safety
# guidance temperature.
T_REF_C = 4.0

# Source: US FoodSafety.gov Cold Food Storage Chart, raw chicken
# (whole or parts): 1-2 days refrigerated. 48h is the upper end of that
# range; 24h is the conservative alternative if you want to be stricter.
L_REF_HOURS = 48.0

# --- Q10 value and its derivation (see SOURCES.md "Q10 derivation") -----

# Mumbre et al.: curd reached 30ppm NH3 (MQ137) at ~day 12 at 4C and
# ~day 4 at 25C, a reported "roughly three times slower" at 4C.
Q10_LITERATURE_RATE_RATIO = 3.0
Q10_LITERATURE_DELTA_T_C = 21.0

# NOTE: Q10_LITERATURE_RATE_RATIO/DELTA_T_C computes to Q10 ~= 1.69, not
# 1.75 (see prediction/q10_model.py:derive_q10_from_ratio and
# SOURCES.md "Q10 derivation"). Left at 1.75 pending a decision - see
# SOURCES.md "Open questions". pipeline.py prints a warning every run
# until this is resolved; it is never silently swapped for the computed
# value.
Q10 = 1.75

# Sensitivity band: since Q10 is borrowed rather than measured, every
# prediction also reports what it would be at these two values, giving
# an earliest/latest use-by range. Our choice, not from a source.
Q10_BAND = (1.5, 2.0)

# Q10 extrapolation is only physically supported in the range the source
# data covers. Outside this, rate_factor() is a guess, not a measurement.
Q10_VALID_TEMP_RANGE_C = (4.0, 25.0)

# --- Forecasting and timestamp handling ----------------------------------

# How far back to average temperature for T_forecast.
FORECAST_WINDOW_HOURS = 12.0

# A gap longer than this (e.g. Wi-Fi outage) is capped so one long outage
# doesn't silently bank hours of "equivalent spoilage" that never happened.
MAX_DT_HOURS = 2.0

# Sensor sanity range for temp_c - readings outside this are a sensor
# fault, not real data, and are dropped. This is NOT a food-safety bound.
PLAUSIBLE_TEMP_RANGE_C = (-10.0, 60.0)

# Set to a specific `datetime` to force a food-start time; leave as None
# to fall back to the earliest reading's timestamp for the trial (see
# pipeline.resolve_food_start). Can also be overridden per run with the
# CLI's --food-start flag.
FOOD_START_TS: Optional[datetime] = None

# --- Secondary indicator: MQ-135 gas ratio --------------------------------

# OPEN QUESTION (see SOURCES.md): reused from food_monitor_compact.ino's
# GAS_DANGER_RATIO so the two systems agree by construction, but the
# MQ-135 was never validated against a real spoilage endpoint for this
# project - Mumbre's 10/30ppm thresholds are for a different sensor
# (MQ137, ammonia-specific) and don't transfer.
MQ135_DANGER_RATIO = 5.0

# --- Status thresholds ----------------------------------------------------

STATUS_USE_SOON_PCT = 70.0
STATUS_PAST_DUE_PCT = 100.0

# Bump this any time a value above changes, so every predictions.csv row
# can be traced back to the exact parameters that produced it.
CONFIG_VERSION = "v1"
```

- [ ] **Step 4: Sanity-check the package imports**

Run: `python -c "from prediction import config, models; print(config.Q10, models.Reading)"`
Expected: prints `1.75 <class 'prediction.models.Reading'>` with no errors.

- [ ] **Step 5: Commit**

```bash
git add prediction/__init__.py prediction/tests/__init__.py prediction/models.py prediction/config.py
git commit -m "feat: scaffold prediction package with data models and config"
```

## Task 2: Q10 math primitives

**Files:**
- Create: `prediction/q10_model.py` (started here; extended in Task 3 and Task 4)
- Test: `prediction/tests/test_q10_model.py` (started here; extended in Task 3 and Task 4)

**Interfaces:**
- Consumes: nothing from earlier tasks except `float`/`str` primitives.
- Produces: `rate_factor(temp_c: float, t_ref_c: float, q10: float) -> float`,
  `derive_q10_from_ratio(rate_ratio: float, delta_t_c: float) -> float`,
  `pct_shelf_life_used(equivalent_hours: float, l_ref_hours: float) -> float`,
  `classify_status(pct_used: float, use_soon_pct: float, past_due_pct: float) -> str`.
  All consumed by `pipeline.py` in Task 8.

- [ ] **Step 1: Write the failing tests**

```python
# prediction/tests/test_q10_model.py
from prediction.q10_model import (
    classify_status,
    derive_q10_from_ratio,
    pct_shelf_life_used,
    rate_factor,
)


def test_rate_factor_is_one_at_reference_temperature():
    assert rate_factor(4.0, t_ref_c=4.0, q10=1.75) == 1.0


def test_rate_factor_is_q10_ten_degrees_above_reference():
    assert abs(rate_factor(14.0, t_ref_c=4.0, q10=1.75) - 1.75) < 1e-6


def test_derive_q10_from_ratio_matches_mumbre_numbers():
    # 12 days / 4 days = 3x slower at 4C across a 21C gap -> Q10 ~= 1.69.
    assert abs(derive_q10_from_ratio(3.0, 21.0) - 1.69) < 0.01


def test_pct_shelf_life_used():
    assert pct_shelf_life_used(24.0, 48.0) == 50.0


def test_classify_status_bands():
    assert classify_status(50.0, use_soon_pct=70.0, past_due_pct=100.0) == "Fresh"
    assert classify_status(80.0, use_soon_pct=70.0, past_due_pct=100.0) == "Use soon"
    assert classify_status(120.0, use_soon_pct=70.0, past_due_pct=100.0) == "Past use-by"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: `ModuleNotFoundError: No module named 'prediction.q10_model'` (or `ImportError`).

- [ ] **Step 3: Write the minimal implementation**

```python
# prediction/q10_model.py
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/q10_model.py prediction/tests/test_q10_model.py
git commit -m "feat: add Q10 rate-factor, derivation, and status primitives"
```

## Task 3: Temperature validation and accumulation

**Files:**
- Modify: `prediction/q10_model.py`
- Modify: `prediction/tests/test_q10_model.py`

**Interfaces:**
- Consumes: `Reading` from `models.py` (Task 1), `rate_factor` from Task 2.
- Produces: `filter_plausible_temperatures(readings: list[Reading], valid_range_c:
  tuple[float, float]) -> tuple[list[Reading], int]`, `Gap` dataclass (`start, end,
  actual_hours, capped_hours`), `AccumulationResult` dataclass (`equivalent_hours: float,
  gaps: list[Gap]`), `accumulate_equivalent_hours(readings: list[Reading], t_ref_c:
  float, q10: float, max_dt_hours: float) -> AccumulationResult`. All consumed by
  `pipeline.py` in Task 8.

- [ ] **Step 1: Write the failing tests**

```python
# append to prediction/tests/test_q10_model.py
from datetime import datetime

from prediction.models import Reading
from prediction.q10_model import accumulate_equivalent_hours, filter_plausible_temperatures


def _reading(ts: datetime, temp_c) -> Reading:
    return Reading(
        server_ts=ts, device_ts=None, trial="T", source="TEST",
        temp_c=temp_c, humidity_pct=None, pressure_hpa=None, lux=None,
        mq135_raw=None, gas_base=None, gas_increase=None, risk=None,
    )


def test_accumulate_equivalent_hours_at_reference_temp():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 10, 0), 4.0),
    ]
    result = accumulate_equivalent_hours(readings, t_ref_c=4.0, q10=1.75, max_dt_hours=24.0)
    assert abs(result.equivalent_hours - 10.0) < 1e-6
    assert result.gaps == []


def test_accumulate_equivalent_hours_caps_long_gaps():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 5, 0), 4.0),  # 5h gap, cap at 2h
    ]
    result = accumulate_equivalent_hours(readings, t_ref_c=4.0, q10=1.75, max_dt_hours=2.0)
    assert abs(result.equivalent_hours - 2.0) < 1e-6
    assert len(result.gaps) == 1
    assert result.gaps[0].actual_hours == 5.0
    assert result.gaps[0].capped_hours == 2.0


def test_filter_plausible_temperatures_drops_missing_and_out_of_range():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 0, 30), None),
        _reading(datetime(2026, 1, 1, 1, 0), 90.0),
    ]
    clean, dropped = filter_plausible_temperatures(readings, (-10.0, 60.0))
    assert len(clean) == 1
    assert dropped == 2
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: the three new tests `FAIL` with `ImportError` (names not yet defined); the
five from Task 2 still `PASS`.

- [ ] **Step 3: Write the minimal implementation**

```python
# append to prediction/q10_model.py
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: 8 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/q10_model.py prediction/tests/test_q10_model.py
git commit -m "feat: add temperature filtering and equivalent-hours accumulation"
```

## Task 4: Forecasting, use-by prediction, and MQ-135 disagreement

**Files:**
- Modify: `prediction/q10_model.py`
- Modify: `prediction/tests/test_q10_model.py`

**Interfaces:**
- Consumes: `Reading` (Task 1), `rate_factor` (Task 2).
- Produces: `forecast_temperature(readings: list[Reading], window_hours: float, now:
  datetime) -> Optional[float]`, `UseByPrediction` dataclass (`remaining_hours:
  float, predicted_use_by: datetime`), `predict_use_by(equivalent_hours: float,
  l_ref_hours: float, forecast_temp_c: float, t_ref_c: float, q10: float, now: datetime)
  -> UseByPrediction`, `mq135_disagrees(status: str, gas_increase: Optional[float],
  danger_ratio: float) -> bool`, `count_outside_q10_range(readings: list[Reading],
  valid_range_c: tuple[float, float]) -> int`. All consumed by `pipeline.py` in Task 8.

- [ ] **Step 1: Write the failing tests**

```python
# append to prediction/tests/test_q10_model.py
from datetime import timedelta

from prediction.q10_model import forecast_temperature, mq135_disagrees, predict_use_by


def test_forecast_temperature_averages_window():
    now = datetime(2026, 1, 2, 0, 0)
    readings = [
        _reading(now - timedelta(hours=1), 20.0),
        _reading(now - timedelta(hours=2), 24.0),
        _reading(now - timedelta(hours=20), 100.0),  # outside the 12h window
    ]
    assert forecast_temperature(readings, window_hours=12.0, now=now) == 22.0


def test_forecast_temperature_returns_none_when_window_is_empty():
    now = datetime(2026, 1, 2, 0, 0)
    readings = [_reading(now - timedelta(hours=20), 100.0)]
    assert forecast_temperature(readings, window_hours=12.0, now=now) is None


def test_predict_use_by_remaining_hours_at_reference_rate():
    now = datetime(2026, 1, 1, 0, 0)
    prediction = predict_use_by(
        equivalent_hours=10.0, l_ref_hours=48.0, forecast_temp_c=4.0,
        t_ref_c=4.0, q10=1.75, now=now,
    )
    assert abs(prediction.remaining_hours - 38.0) < 1e-6
    assert prediction.predicted_use_by == now + timedelta(hours=38.0)


def test_mq135_disagrees_only_when_model_says_fresh_but_sensor_says_danger():
    assert mq135_disagrees("Fresh", gas_increase=6.0, danger_ratio=5.0) is True
    assert mq135_disagrees("Fresh", gas_increase=1.0, danger_ratio=5.0) is False
    assert mq135_disagrees("Use soon", gas_increase=6.0, danger_ratio=5.0) is False
    assert mq135_disagrees("Fresh", gas_increase=None, danger_ratio=5.0) is False


def test_count_outside_q10_range_counts_readings_outside_the_source_data_range():
    from prediction.q10_model import count_outside_q10_range

    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),   # inside (4.0, 25.0)
        _reading(datetime(2026, 1, 1, 1, 0), 3.9),   # below range
        _reading(datetime(2026, 1, 1, 2, 0), 30.0),  # above range
        _reading(datetime(2026, 1, 1, 3, 0), None),  # missing, not counted here
    ]
    assert count_outside_q10_range(readings, (4.0, 25.0)) == 2
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: the five new tests `FAIL` with `ImportError`; the eight from Tasks 2-3 still
`PASS`.

- [ ] **Step 3: Write the minimal implementation**

```python
# append to prediction/q10_model.py
from typing import Optional


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
```

Also add `from datetime import timedelta` to the existing `from datetime import
datetime` import at the top of `prediction/q10_model.py` (combine into one import line).

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest prediction/tests/test_q10_model.py -v`
Expected: 13 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/q10_model.py prediction/tests/test_q10_model.py
git commit -m "feat: add forecasting, use-by prediction, and MQ-135 disagreement check"
```

## Task 5: HTTPS-payload CSV loader (mock Sheet schema)

**Files:**
- Create: `prediction/data_io.py` (started here; extended in Tasks 6-7)
- Create: `prediction/tests/test_data_io.py` (started here; extended in Tasks 6-7)

**Interfaces:**
- Consumes: `Reading` from `models.py` (Task 1).
- Produces: `load_https_payload_csv(path: str) -> list[Reading]`. Consumed by
  `pipeline.py` (Task 8) and `cli.py` (Task 9).

- [ ] **Step 1: Write the failing test**

```python
# prediction/tests/test_data_io.py
from datetime import datetime

from prediction.data_io import load_https_payload_csv

HTTPS_CSV = """server_ts,device_ts,trial,source,temp,humidity,pressure,lux,mq135,gas_base,gas_increase,risk
2026-10-01T09:00:00,2026-10-01T09:00:01,ChickenTrial1,MANUAL,4.0,55.0,1015.0,0.0,900,900,1.0,SAFE
2026-10-01T09:30:00,,ChickenTrial1,SCHEDULED,,55.0,1015.0,0.0,910,900,1.011,SAFE
"""


def test_load_https_payload_csv_parses_known_fields_and_blanks(tmp_path):
    path = tmp_path / "sheet.csv"
    path.write_text(HTTPS_CSV, encoding="utf-8")

    readings = load_https_payload_csv(str(path))

    assert len(readings) == 2
    first = readings[0]
    assert first.server_ts == datetime(2026, 10, 1, 9, 0, 0)
    assert first.trial == "ChickenTrial1"
    assert first.temp_c == 4.0
    assert first.gas_increase == 1.0
    second = readings[1]
    assert second.temp_c is None  # blank field -> None, not an error
    assert second.device_ts is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: `ModuleNotFoundError: No module named 'prediction.data_io'`.

- [ ] **Step 3: Write the minimal implementation**

```python
# prediction/data_io.py
"""Reading and writing the CSVs the prediction pipeline runs on.

All I/O lives here so q10_model.py and pipeline.py stay pure/testable.
Two input schemas are supported because the real Google Sheet doesn't
exist yet (see PIPELINE_PLAN.md): the HTTPS payload schema the firmware
already sends (load_https_payload_csv) and the on-device DATA.CSV
schema as a backup (load_sd_datacsv, Task 6). When the real Sheet
exists, it should be fetched the same way prediction's sibling script
`read-data-from-google-sheet` already does (public CSV export URL) and
fed through load_https_payload_csv unchanged, since the Apps Script's
actual column layout is still unverified (PIPELINE_PLAN.md, open risk
#1) - see SOURCES.md "Open questions".
"""
from __future__ import annotations

import csv
from datetime import datetime
from typing import Optional

from prediction.models import Reading


def _parse_float(value: str) -> Optional[float]:
    value = value.strip()
    if not value:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def _parse_ts(value: str) -> Optional[datetime]:
    value = value.strip()
    if not value:
        return None
    return datetime.fromisoformat(value)


def load_https_payload_csv(path: str) -> list[Reading]:
    """Load readings logged in the HTTPS-payload schema (mock Sheet).

    Expected header (order doesn't matter, matched by name):
    server_ts, device_ts, trial, source, temp, humidity, pressure, lux,
    mq135, gas_base, gas_increase, risk.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            readings.append(Reading(
                server_ts=_parse_ts(row["server_ts"]),
                device_ts=_parse_ts(row.get("device_ts", "")),
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp", "")),
                humidity_pct=_parse_float(row.get("humidity", "")),
                pressure_hpa=_parse_float(row.get("pressure", "")),
                lux=_parse_float(row.get("lux", "")),
                mq135_raw=_parse_float(row.get("mq135", "")),
                gas_base=_parse_float(row.get("gas_base", "")),
                gas_increase=_parse_float(row.get("gas_increase", "")),
                risk=row.get("risk") or None,
            ))
    return readings
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: 1 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/data_io.py prediction/tests/test_data_io.py
git commit -m "feat: add HTTPS-payload-schema CSV loader"
```

## Task 6: SD DATA.CSV backup loader

**Files:**
- Modify: `prediction/data_io.py`
- Modify: `prediction/tests/test_data_io.py`

**Interfaces:**
- Consumes: `Reading` (Task 1).
- Produces: `load_sd_datacsv(path: str) -> list[Reading]`. Consumed by `cli.py`
  (Task 9) as the `--format datacsv` option.

- [ ] **Step 1: Write the failing test**

```python
# append to prediction/tests/test_data_io.py
from prediction.data_io import load_sd_datacsv

SD_CSV = """date,time,trial,temp_C,humidity_pct,pressure_hPa,lux,mq135_raw,mq135_base,mq135_increase,source
2026-10-01,09:00:00,ChickenTrial1,4.00,55.00,1015.00,0.0,900,900,1.000,MANUAL
"""


def test_load_sd_datacsv_combines_date_and_time(tmp_path):
    path = tmp_path / "data.csv"
    path.write_text(SD_CSV, encoding="utf-8")

    readings = load_sd_datacsv(str(path))

    assert len(readings) == 1
    r = readings[0]
    assert r.server_ts == datetime(2026, 10, 1, 9, 0, 0)
    assert r.temp_c == 4.0
    assert r.gas_base == 900.0
    assert r.gas_increase == 1.0
    assert r.device_ts is None
    assert r.risk is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: the new test `FAIL`s with `ImportError`; the Task 5 test still `PASS`es.

- [ ] **Step 3: Write the minimal implementation**

```python
# append to prediction/data_io.py
def load_sd_datacsv(path: str) -> list[Reading]:
    """Load readings from the on-device DATA.CSV schema (backup path).

    Expected header: date,time,trial,temp_C,humidity_pct,pressure_hPa,
    lux,mq135_raw,mq135_base,mq135_increase,source. There is no
    device_ts or risk column in this schema, so those fields are None.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            server_ts = datetime.strptime(
                f"{row['date']} {row['time']}", "%Y-%m-%d %H:%M:%S"
            )
            readings.append(Reading(
                server_ts=server_ts,
                device_ts=None,
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp_C", "")),
                humidity_pct=_parse_float(row.get("humidity_pct", "")),
                pressure_hpa=_parse_float(row.get("pressure_hPa", "")),
                lux=_parse_float(row.get("lux", "")),
                mq135_raw=_parse_float(row.get("mq135_raw", "")),
                gas_base=_parse_float(row.get("mq135_base", "")),
                gas_increase=_parse_float(row.get("mq135_increase", "")),
                risk=None,
            ))
    return readings
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: 2 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/data_io.py prediction/tests/test_data_io.py
git commit -m "feat: add SD DATA.CSV backup loader"
```

## Task 7: Predictions CSV writer

**Files:**
- Modify: `prediction/data_io.py`
- Modify: `prediction/tests/test_data_io.py`

**Interfaces:**
- Consumes: `PredictionRow` from `models.py` (Task 1).
- Produces: `write_prediction_row(path: str, row: PredictionRow) -> None`. Consumed by
  `cli.py` (Task 9).

- [ ] **Step 1: Write the failing test**

```python
# append to prediction/tests/test_data_io.py
from prediction.data_io import write_prediction_row
from prediction.models import PredictionRow


def _sample_row() -> PredictionRow:
    return PredictionRow(
        run_ts=datetime(2026, 10, 2, 9, 0, 0), equivalent_hours=10.0,
        pct_shelf_life_used=20.8, remaining_hours=38.0,
        predicted_use_by=datetime(2026, 10, 3, 23, 0, 0),
        predicted_use_by_earliest=datetime(2026, 10, 3, 12, 0, 0),
        predicted_use_by_latest=datetime(2026, 10, 4, 10, 0, 0),
        status="Fresh", mq135_gas_increase=1.2, mq135_disagrees=False,
        gap_count=0, dropped_reading_count=0, q10_range_exceeded_count=0,
        t_ref_c=4.0, l_ref_hours=48.0,
        q10=1.75, q10_band_low=1.5, q10_band_high=2.0,
        forecast_window_hours=12.0, config_version="v1",
    )


def test_write_prediction_row_writes_header_once_then_appends(tmp_path):
    path = tmp_path / "predictions.csv"

    write_prediction_row(str(path), _sample_row())
    write_prediction_row(str(path), _sample_row())

    lines = path.read_text(encoding="utf-8").strip().splitlines()
    assert lines[0].startswith("run_ts,equivalent_hours")
    assert len(lines) == 3  # header + two appended rows
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: the new test `FAIL`s with `ImportError`; the Task 5-6 tests still `PASS`.

- [ ] **Step 3: Write the minimal implementation**

```python
# append to prediction/data_io.py
import os

from prediction.models import PredictionRow

_PREDICTION_FIELDS = [
    "run_ts", "equivalent_hours", "pct_shelf_life_used", "remaining_hours",
    "predicted_use_by", "predicted_use_by_earliest", "predicted_use_by_latest",
    "status", "mq135_gas_increase", "mq135_disagrees", "gap_count",
    "dropped_reading_count", "q10_range_exceeded_count", "t_ref_c", "l_ref_hours",
    "q10", "q10_band_low", "q10_band_high", "forecast_window_hours", "config_version",
]


def write_prediction_row(path: str, row: PredictionRow) -> None:
    """Append one prediction to the predictions CSV, writing a header first."""
    is_new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if is_new:
            writer.writerow(_PREDICTION_FIELDS)
        writer.writerow([
            row.run_ts.isoformat(),
            row.equivalent_hours,
            row.pct_shelf_life_used,
            row.remaining_hours,
            row.predicted_use_by.isoformat(),
            row.predicted_use_by_earliest.isoformat(),
            row.predicted_use_by_latest.isoformat(),
            row.status,
            row.mq135_gas_increase,
            row.mq135_disagrees,
            row.gap_count,
            row.dropped_reading_count,
            row.q10_range_exceeded_count,
            row.t_ref_c,
            row.l_ref_hours,
            row.q10,
            row.q10_band_low,
            row.q10_band_high,
            row.forecast_window_hours,
            row.config_version,
        ])
```

Move the `import os` and `from prediction.models import PredictionRow` lines up to the
top of the file alongside the existing imports rather than leaving them mid-file.

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest prediction/tests/test_data_io.py -v`
Expected: 3 passed.

- [ ] **Step 5: Commit**

```bash
git add prediction/data_io.py prediction/tests/test_data_io.py
git commit -m "feat: add predictions CSV writer"
```

## Task 8: Pipeline orchestration + mock trial data

**Files:**
- Create: `prediction/pipeline.py`
- Create: `prediction/mock_data/mock_sheet.csv`
- Create: `prediction/tests/test_pipeline.py`

**Interfaces:**
- Consumes: everything from Tasks 1-7 (`Reading`, `PredictionRow`,
  `filter_plausible_temperatures`, `accumulate_equivalent_hours`,
  `forecast_temperature`, `predict_use_by`, `pct_shelf_life_used`, `classify_status`,
  `mq135_disagrees`, `derive_q10_from_ratio`, `count_outside_q10_range`, `config.*`).
- Produces: `resolve_food_start(readings: list[Reading], configured_start:
  Optional[datetime], trial: Optional[str]) -> datetime`,
  `q10_discrepancy_message() -> Optional[str]`, `run_prediction(readings: list[Reading],
  now: datetime) -> PredictionRow`. All consumed by `cli.py` (Task 9).

- [ ] **Step 1: Write the mock trial data**

Create `prediction/mock_data/mock_sheet.csv` with this exact content. It simulates one
trial (`ChickenTrial1`): ~2.5 hours refrigerated, a 5-hour Wi-Fi outage (to exercise gap
capping), one blank-temperature row and one 90°C sensor-glitch row (to exercise
plausibility filtering), then a move to room temperature that pushes `gas_increase`
across the 5.0 danger ratio while the Q10 model still reports "Fresh" (to exercise the
MQ-135 disagreement flag):

```csv
server_ts,device_ts,trial,source,temp,humidity,pressure,lux,mq135,gas_base,gas_increase,risk
2026-10-01T09:00:00,2026-10-01T09:00:01,ChickenTrial1,STARTUP,4.0,55.0,1015.0,0.0,905,900,1.006,SAFE
2026-10-01T09:30:00,2026-10-01T09:30:01,ChickenTrial1,SCHEDULED,4.1,55.0,1015.0,0.0,910,900,1.011,SAFE
2026-10-01T10:00:00,2026-10-01T10:00:01,ChickenTrial1,SCHEDULED,4.0,55.0,1015.0,0.0,900,900,1.000,SAFE
2026-10-01T10:30:00,2026-10-01T10:30:01,ChickenTrial1,SCHEDULED,3.9,55.0,1015.0,0.0,895,900,0.994,SAFE
2026-10-01T11:00:00,2026-10-01T11:00:01,ChickenTrial1,SCHEDULED,,55.0,1015.0,0.0,0,900,0.000,SAFE
2026-10-01T11:30:00,2026-10-01T11:30:01,ChickenTrial1,SCHEDULED,4.2,55.0,1015.0,0.0,905,900,1.006,SAFE
2026-10-01T16:30:00,2026-10-01T16:30:01,ChickenTrial1,SCHEDULED,4.5,55.0,1015.0,0.0,930,900,1.033,SAFE
2026-10-01T17:00:00,2026-10-01T17:00:01,ChickenTrial1,SCHEDULED,4.4,55.0,1015.0,0.0,940,900,1.044,SAFE
2026-10-01T17:30:00,2026-10-01T17:30:01,ChickenTrial1,SCHEDULED,22.0,45.0,1013.0,120.0,1500,900,1.667,CAUTION
2026-10-01T18:00:00,2026-10-01T18:00:01,ChickenTrial1,SCHEDULED,22.3,45.0,1013.0,120.0,2200,900,2.444,CAUTION
2026-10-01T18:30:00,2026-10-01T18:30:01,ChickenTrial1,SCHEDULED,22.5,45.0,1013.0,120.0,3400,900,3.778,CAUTION
2026-10-01T19:00:00,2026-10-01T19:00:01,ChickenTrial1,SCHEDULED,22.6,45.0,1013.0,120.0,4600,900,5.111,DANGER
2026-10-01T19:30:00,2026-10-01T19:30:01,ChickenTrial1,SCHEDULED,90.0,45.0,1013.0,120.0,4650,900,5.167,DANGER
2026-10-01T20:00:00,2026-10-01T20:00:01,ChickenTrial1,SCHEDULED,22.7,45.0,1013.0,120.0,4700,900,5.222,DANGER
2026-10-01T20:30:00,2026-10-01T20:30:01,ChickenTrial1,SCHEDULED,22.8,45.0,1013.0,120.0,4750,900,5.278,DANGER
```

Note the deliberate blank `temp` field at `11:00:00` and the `90.0` reading at
`19:30:00` — both must be dropped by `filter_plausible_temperatures`, which is why
`dropped_reading_count == 2` in the test below. The `16:30:00` row follows a ~5h gap
from `11:30:00`, which is why `gap_count == 1`. The `10:30:00` reading (3.9°C) is the
only clean reading below `config.Q10_VALID_TEMP_RANGE_C`'s lower bound of 4.0°C, which
is why `q10_range_exceeded_count == 1`.

- [ ] **Step 2: Write the failing tests**

```python
# prediction/tests/test_pipeline.py
from datetime import datetime

from prediction.data_io import load_https_payload_csv
from prediction.pipeline import resolve_food_start, run_prediction

MOCK_SHEET = "prediction/mock_data/mock_sheet.csv"


def test_resolve_food_start_falls_back_to_earliest_reading():
    readings = load_https_payload_csv(MOCK_SHEET)
    start = resolve_food_start(readings, configured_start=None, trial="ChickenTrial1")
    assert start == datetime(2026, 10, 1, 9, 0, 0)


def test_run_prediction_on_mock_trial_data():
    readings = load_https_payload_csv(MOCK_SHEET)
    start = resolve_food_start(readings, configured_start=None, trial="ChickenTrial1")
    readings = sorted(
        (r for r in readings if r.trial == "ChickenTrial1" and r.server_ts >= start),
        key=lambda r: r.server_ts,
    )
    now = datetime(2026, 10, 1, 21, 0, 0)

    row = run_prediction(readings, now)

    assert row.dropped_reading_count == 2  # the blank-temp and 90C rows
    assert row.gap_count == 1              # the 5h outage, capped at 2h
    assert row.q10_range_exceeded_count == 1  # the 3.9C reading, below 4.0C
    assert row.status == "Fresh"
    assert row.mq135_disagrees is True     # model says Fresh, sensor says DANGER
    assert 25.0 <= row.pct_shelf_life_used <= 40.0
    assert row.predicted_use_by_earliest < row.predicted_use_by < row.predicted_use_by_latest
    assert row.config_version == "v1"
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `python -m pytest prediction/tests/test_pipeline.py -v`
Expected: `ModuleNotFoundError: No module named 'prediction.pipeline'`.

- [ ] **Step 4: Write the minimal implementation**

```python
# prediction/pipeline.py
"""Wires the pure Q10 math (q10_model.py) to real readings (data_io.py).

This is the only module that knows about config.py's values; q10_model.py
takes every parameter as an explicit argument so it stays testable
without a config file at all.
"""
from __future__ import annotations

from datetime import datetime
from typing import Optional

from prediction import config
from prediction.models import PredictionRow, Reading
from prediction.q10_model import (
    accumulate_equivalent_hours,
    classify_status,
    count_outside_q10_range,
    derive_q10_from_ratio,
    filter_plausible_temperatures,
    forecast_temperature,
    mq135_disagrees,
    pct_shelf_life_used,
    predict_use_by,
)


def resolve_food_start(
    readings: list[Reading], configured_start: Optional[datetime], trial: Optional[str]
) -> datetime:
    """config.FOOD_START_TS (or a CLI override) wins; else earliest reading."""
    if configured_start is not None:
        return configured_start
    candidates = [r for r in readings if trial is None or r.trial == trial]
    if not candidates:
        raise ValueError("No readings to resolve a food-start timestamp from.")
    return min(r.server_ts for r in candidates)


def q10_discrepancy_message() -> Optional[str]:
    """Non-None whenever config.Q10 doesn't match the value its own
    stored literature inputs compute to - see config.py's Q10 comment."""
    computed = derive_q10_from_ratio(
        config.Q10_LITERATURE_RATE_RATIO, config.Q10_LITERATURE_DELTA_T_C
    )
    if abs(computed - config.Q10) > 0.01:
        return (
            f"NOTE: config.Q10={config.Q10} but the stored Mumbre inputs "
            f"(ratio={config.Q10_LITERATURE_RATE_RATIO}, "
            f"dT={config.Q10_LITERATURE_DELTA_T_C}C) compute to "
            f"Q10={computed:.3f}. See SOURCES.md 'Q10 derivation' and "
            f"'Open questions' before trusting this prediction."
        )
    return None


def run_prediction(readings: list[Reading], now: datetime) -> PredictionRow:
    """Readings must already be filtered to one trial, sorted ascending,
    and starting at the resolved food-start timestamp."""
    clean, dropped = filter_plausible_temperatures(readings, config.PLAUSIBLE_TEMP_RANGE_C)

    forecast_temp = forecast_temperature(clean, config.FORECAST_WINDOW_HOURS, now)
    if forecast_temp is None:
        forecast_temp = config.T_REF_C

    central_acc = accumulate_equivalent_hours(
        clean, config.T_REF_C, config.Q10, config.MAX_DT_HOURS
    )
    central = predict_use_by(
        central_acc.equivalent_hours, config.L_REF_HOURS, forecast_temp,
        config.T_REF_C, config.Q10, now,
    )

    # Lower Q10 -> slower spoilage -> later use-by ("latest").
    # Higher Q10 -> faster spoilage -> earlier use-by ("earliest").
    q10_low, q10_high = config.Q10_BAND
    latest_acc = accumulate_equivalent_hours(
        clean, config.T_REF_C, q10_low, config.MAX_DT_HOURS
    )
    latest = predict_use_by(
        latest_acc.equivalent_hours, config.L_REF_HOURS, forecast_temp,
        config.T_REF_C, q10_low, now,
    )
    earliest_acc = accumulate_equivalent_hours(
        clean, config.T_REF_C, q10_high, config.MAX_DT_HOURS
    )
    earliest = predict_use_by(
        earliest_acc.equivalent_hours, config.L_REF_HOURS, forecast_temp,
        config.T_REF_C, q10_high, now,
    )

    pct_used = pct_shelf_life_used(central_acc.equivalent_hours, config.L_REF_HOURS)
    status = classify_status(pct_used, config.STATUS_USE_SOON_PCT, config.STATUS_PAST_DUE_PCT)
    latest_gas_increase = clean[-1].gas_increase if clean else None
    disagrees = mq135_disagrees(status, latest_gas_increase, config.MQ135_DANGER_RATIO)
    out_of_range = count_outside_q10_range(clean, config.Q10_VALID_TEMP_RANGE_C)

    return PredictionRow(
        run_ts=now,
        equivalent_hours=central_acc.equivalent_hours,
        pct_shelf_life_used=pct_used,
        remaining_hours=central.remaining_hours,
        predicted_use_by=central.predicted_use_by,
        predicted_use_by_earliest=earliest.predicted_use_by,
        predicted_use_by_latest=latest.predicted_use_by,
        status=status,
        mq135_gas_increase=latest_gas_increase,
        mq135_disagrees=disagrees,
        gap_count=len(central_acc.gaps),
        dropped_reading_count=dropped,
        q10_range_exceeded_count=out_of_range,
        t_ref_c=config.T_REF_C,
        l_ref_hours=config.L_REF_HOURS,
        q10=config.Q10,
        q10_band_low=q10_low,
        q10_band_high=q10_high,
        forecast_window_hours=config.FORECAST_WINDOW_HOURS,
        config_version=config.CONFIG_VERSION,
    )
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `python -m pytest prediction/tests/test_pipeline.py -v`
Expected: 2 passed. If `test_run_prediction_on_mock_trial_data` fails on the
`pct_shelf_life_used` bounds, print `row.pct_shelf_life_used` and sanity-check against
the hand-calculation in this plan's design notes before widening the bounds — a wildly
different number likely means a bug in `accumulate_equivalent_hours`, not a bad test
bound.

- [ ] **Step 6: Run the full test suite so far**

Run: `python -m pytest prediction/tests -v`
Expected: 18 passed (5 + 3 + 5 + 1 + 1 + 1 + 2).

- [ ] **Step 7: Commit**

```bash
git add prediction/pipeline.py prediction/mock_data/mock_sheet.csv prediction/tests/test_pipeline.py
git commit -m "feat: add pipeline orchestration and mock trial data"
```

## Task 9: CLI entry point

**Files:**
- Create: `prediction/cli.py`
- Create: `prediction/tests/test_cli.py`

**Interfaces:**
- Consumes: `config`, `load_https_payload_csv`/`load_sd_datacsv`/`write_prediction_row`
  (Tasks 5-7), `resolve_food_start`/`q10_discrepancy_message`/`run_prediction` (Task 8).
- Produces: `main(argv: Optional[list[str]] = None) -> None`, runnable as `python -m
  prediction.cli` and schedulable via cron/Task Scheduler.

- [ ] **Step 1: Write the failing test**

```python
# prediction/tests/test_cli.py
from prediction.cli import main

MOCK_SHEET = "prediction/mock_data/mock_sheet.csv"


def test_cli_runs_end_to_end_and_writes_predictions(tmp_path, capsys):
    out_path = tmp_path / "predictions.csv"
    main([
        "--readings", MOCK_SHEET,
        "--format", "https",
        "--predictions-out", str(out_path),
        "--trial", "ChickenTrial1",
        "--now", "2026-10-01T21:00:00",
    ])

    captured = capsys.readouterr()
    assert "Status: Fresh" in captured.out
    assert "WARNING: MQ-135 disagrees" in captured.out
    assert "NOTE: 1 reading(s) fell outside the Q10-supported" in captured.out
    assert out_path.exists()
    assert len(out_path.read_text(encoding="utf-8").strip().splitlines()) == 2  # header + 1 row
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m pytest prediction/tests/test_cli.py -v`
Expected: `ModuleNotFoundError: No module named 'prediction.cli'`.

- [ ] **Step 3: Write the minimal implementation**

```python
# prediction/cli.py
"""Command-line entry point: predict a use-by date from a readings CSV.

Run once or twice a day (cron on Linux/macOS, Task Scheduler on
Windows) - see README.md "Scheduling" for exact commands. Example:

    python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1
"""
from __future__ import annotations

import argparse
from datetime import datetime
from typing import Optional

from prediction import config
from prediction.data_io import load_https_payload_csv, load_sd_datacsv, write_prediction_row
from prediction.pipeline import q10_discrepancy_message, resolve_food_start, run_prediction

_LOADERS = {
    "https": load_https_payload_csv,
    "datacsv": load_sd_datacsv,
}


def main(argv: Optional[list[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="Predict a use-by date from logged sensor readings.")
    parser.add_argument("--readings", required=True, help="path to the readings CSV")
    parser.add_argument(
        "--format", choices=["https", "datacsv"], default="https",
        help="https = mock/real Sheet schema (default), datacsv = SD card DATA.CSV schema",
    )
    parser.add_argument("--predictions-out", default="predictions.csv", help="CSV to append the result to")
    parser.add_argument("--trial", default=None, help="only use readings for this trial name")
    parser.add_argument(
        "--food-start", default=None,
        help="ISO8601 override for when the food went in (else config.FOOD_START_TS or earliest reading)",
    )
    parser.add_argument("--now", default=None, help="ISO8601 override for 'now', for testing")
    args = parser.parse_args(argv)

    warning = q10_discrepancy_message()
    if warning:
        print(warning)

    readings = _LOADERS[args.format](args.readings)
    if args.trial:
        readings = [r for r in readings if r.trial == args.trial]

    now = datetime.fromisoformat(args.now) if args.now else datetime.now()
    configured_start = (
        datetime.fromisoformat(args.food_start) if args.food_start else config.FOOD_START_TS
    )
    start = resolve_food_start(readings, configured_start, args.trial)
    readings = sorted((r for r in readings if r.server_ts >= start), key=lambda r: r.server_ts)

    row = run_prediction(readings, now)
    write_prediction_row(args.predictions_out, row)

    print(f"Status: {row.status}")
    print(
        f"Equivalent hours: {row.equivalent_hours:.1f} / {row.l_ref_hours:.0f} "
        f"({row.pct_shelf_life_used:.0f}% of shelf life used)"
    )
    print(
        f"Predicted use-by: {row.predicted_use_by.isoformat()} "
        f"(range {row.predicted_use_by_earliest.isoformat()} to "
        f"{row.predicted_use_by_latest.isoformat()})"
    )
    print(f"MQ-135 gas increase: {row.mq135_gas_increase}")
    if row.mq135_disagrees:
        print("WARNING: MQ-135 disagrees with the model's Fresh status.")
    if row.gap_count:
        print(f"NOTE: {row.gap_count} reading gap(s) longer than {config.MAX_DT_HOURS}h were capped.")
    if row.dropped_reading_count:
        print(f"NOTE: {row.dropped_reading_count} reading(s) dropped for an implausible temperature.")
    if row.q10_range_exceeded_count:
        print(
            f"NOTE: {row.q10_range_exceeded_count} reading(s) fell outside the "
            f"Q10-supported temperature range {config.Q10_VALID_TEMP_RANGE_C} - "
            f"those intervals are extrapolated, not verified."
        )


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m pytest prediction/tests/test_cli.py -v`
Expected: 1 passed.

- [ ] **Step 5: Run the full test suite**

Run: `python -m pytest prediction/tests -v`
Expected: 19 passed.

- [ ] **Step 6: Commit**

```bash
git add prediction/cli.py prediction/tests/test_cli.py
git commit -m "feat: add CLI entry point for scheduled use-by predictions"
```

## Task 10: README, SOURCES, and open questions

**Files:**
- Create: `prediction/README.md`
- Create: `prediction/SOURCES.md`

**Interfaces:**
- Consumes: nothing (documentation only).
- Produces: nothing consumed by code; this is the human-facing deliverable the report
  will cite.

- [ ] **Step 1: Write `prediction/README.md`**

```markdown
# Q10 shelf-life prediction

Predicts a use-by date for raw chicken from logged ESP32 sensor readings, using a
literature-derived Q10 temperature-acceleration model. See `SOURCES.md` for every
parameter's source, justification, and open questions.

## Usage

    pip install pytest          # only needed to run the tests
    python -m prediction.cli --readings path/to/readings.csv --trial ChickenTrial1

By default this reads the HTTPS-payload schema (what `food_monitor_compact.ino`'s
`publishToGoogleSheets()` sends). Pass `--format datacsv` to read the on-device
`DATA.CSV` schema instead. Results are appended to `predictions.csv` (override with
`--predictions-out`).

The real Google Sheet doesn't exist for us yet (a teammate holds the link) - once it
does, export it as CSV (File > Download > CSV, or the public `.../export?format=csv`
URL the same way `read-data-from-google-sheet` reads it) and point `--readings` at that
file. No code change is needed as long as the Apps Script's columns match the HTTPS
schema above.

## Scheduling

Run once or twice a day:

- **Linux/macOS (cron):** `0 8,20 * * * cd /path/to/repo && python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1`
- **Windows (Task Scheduler):** create a daily trigger (or two) that runs
  `python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1` with
  "Start in" set to the repo root.

## Running the tests

    python -m pytest prediction/tests -v

## Limitations

- **Q10 is borrowed, not measured.** It comes from Mumbre et al.'s curd data, not a
  chicken trial. See `SOURCES.md` "Q10 derivation" for the unresolved 1.69-vs-1.75
  discrepancy this implies.
- **No microbial lag phase.** The model likely over-predicts spoilage early in storage,
  since real spoilage has a slow-start phase Q10 doesn't capture.
- **Q10 assumes a constant rate-temperature relationship**, which breaks down outside
  roughly 4-25°C (`config.Q10_VALID_TEMP_RANGE_C`). Readings outside that range are
  still used in the accumulation (not dropped), but are counted
  (`q10_range_exceeded_count`) and the CLI prints a warning when any occur.
- **T_forecast is a guess** based on the mean of the last `config.FORECAST_WINDOW_HOURS`
  of readings, not an actual forecast.
- **MQ-135 vs. ammonia-specific MQ137.** Mumbre's 10/30ppm ammonia thresholds don't
  transfer to our broader-spectrum MQ-135; `config.MQ135_DANGER_RATIO` is a reused
  firmware value, not a validated one. See `SOURCES.md` "Open questions".

## Assumptions

- The real Apps Script / Google Sheet is unverified (`PIPELINE_PLAN.md`, open risk #1).
  All development and testing here uses the mock data in `mock_data/mock_sheet.csv`.
- A food-start timestamp is required to accumulate equivalent hours correctly; without
  one, `resolve_food_start` falls back to the trial's earliest reading, which may
  include readings from before the chicken was actually placed in the enclosure.
```

- [ ] **Step 2: Write `prediction/SOURCES.md`**

```markdown
# Sources and parameter derivations

IEEE-style citation log for every literature-derived parameter in `config.py`. See
`README.md` "Limitations" for what each of these costs the model.

## References

1. T. Mumbre, T. Salunkhe, and R. Kadu, "IoT-Based Food Spoilage Detection System using
   Ammonia Sensing and Environmental Monitoring," *International Journal of
   Engineering Research & Technology (IJERT)*, vol. 15, no. 04, Apr. 2026, paper ID
   IJERTV15IS042618, doi: 10.5281/zenodo.19821237. [Online]. Available:
   https://www.ijert.org/iot-based-food-spoilage-detection-system-using-ammonia-sensing-and-environmental-monitoring-ijertv15is042618
2. P. S. Taoukis, T. P. Labuza, and I. S. Saguy, *Kinetics of Food Deterioration and
   Shelf-Life Prediction*. Boca Raton, FL, USA: CRC Press, 1997.
3. U.S. Food Safety and Inspection Service, "Cold Food Storage Chart," FoodSafety.gov.
   [Online]. Available: **TODO (Marcus): insert the verified primary FoodSafety.gov URL
   here before submission - do not cite a secondary article.**

## Parameter table

| Parameter | Value | Source | Justification |
|---|---|---|---|
| `T_REF_C` | 4.0 | [1] (refrigerated condition); [3] (chicken guidance, <=4C/40F) | Matches both the Q10 dataset and the shelf-life guidance temperature |
| `L_REF_HOURS` | 48.0 | [3], raw chicken: 1-2 days refrigerated | Chicken-specific, official food-safety authority. 48h is the upper end of the range; 24h is the conservative alternative |
| `Q10` | 1.75 | Derived from [1] via the formula in [2] | See "Q10 derivation" below - this value does not match what [1]'s own numbers compute to |
| `Q10_BAND` | (1.5, 2.0) | Our choice | Gives an earliest/latest use-by because Q10 is borrowed, not measured |
| `MQ135_DANGER_RATIO` | 5.0 | Reused from `food_monitor_compact.ino`'s `GAS_DANGER_RATIO` | Not independently validated - see "Open questions" |

## Q10 derivation

[1] reports curd (not chicken) reaching 30ppm NH3 (via an MQ137 ammonia sensor) at
approximately day 12 at 4°C and day 4 at 25°C - a reported "roughly three times
slower" rate at 4°C. Using [2]'s formula:

    Q10 = (rate ratio) ^ (10 / delta_T) = 3.0 ^ (10 / 21) ~= 1.69

This is implemented as `q10_model.derive_q10_from_ratio(3.0, 21.0)` and unit-tested in
`tests/test_q10_model.py`. **`config.Q10` is set to 1.75, not this computed 1.69** - see
"Open questions" below. `pipeline.q10_discrepancy_message()` prints both numbers every
time the CLI runs so this is never silently resolved one way or the other.

## Limitations (see also README.md)

- **Food mismatch:** [1]'s Q10 comes from curd, not chicken - borrowed for temperature
  sensitivity only, not for shelf life.
- **Temperature range:** supported only between ~4°C and ~25°C ([1]'s tested range);
  `config.Q10_VALID_TEMP_RANGE_C` records this, but readings outside it are still used.
- **Different endpoints:** [1]'s endpoint is a measured ammonia threshold; `L_REF_HOURS`
  is a food-safety guideline ([3]). A use-by date is arguably better served by the
  safety-based limit, but the two are not the same kind of measurement.
- **Sensor mismatch:** [1] used an MQ137 (ammonia-specific); this project uses an
  MQ-135 (broad VOC). [1]'s 10/30ppm thresholds do not transfer to MQ-135 readings.
- **No microbial lag phase**, and **T_forecast is a guess** from recent history, not an
  actual weather/environment forecast.

## Replacement plan

Stage 1 reference trials (chicken held at 4°C and 20°C) can give an empirical
`L_REF_HOURS` and `Q10 = (t_cold / t_warm) ^ (10 / dT)` once that data exists, computed
the same way `q10_model.derive_q10_from_ratio` already does. No task in this plan
implements the trial-data ingestion itself - see "Open questions".

## Open questions

1. **Q10: 1.75 vs. ≈1.69.** `config.Q10 = 1.75` is the literature-asserted value, but
   deriving it from [1]'s own reported numbers (3x slower, 21°C gap) via [2]'s formula
   gives ≈1.69. A ratio of ≈3.24x would be needed to get 1.75. Decide which to adopt, or
   re-derive from a closer read of [1] Fig. 3.
2. **MQ-135 disagreement threshold.** `config.MQ135_DANGER_RATIO = 5.0` is reused from
   the firmware's existing gas-ratio bands so the two systems agree by construction, but
   it was never validated against [1]'s ammonia data (different sensor, different food).
   Decide if 5.0 is right or if an independent threshold is needed.
3. **FoodSafety.gov primary URL.** Reference [3] above needs the verified primary page
   URL - retrieve and insert it before submission; do not cite a secondary article.
4. **Conservative `L_REF_HOURS`.** [3]'s range is 1-2 days; `config.L_REF_HOURS = 48.0`
   uses the upper end. Decide if a separate, stricter config value (24h) is worth adding
   now or only once real trial data suggests it's needed.
5. **Symmetric MQ-135 disagreement.** `q10_model.mq135_disagrees` only flags "model says
   Fresh, sensor says danger" (the safety-relevant direction). Decide if the reverse
   (model says Use soon/Past use-by, sensor reads well under the danger ratio) is also
   worth flagging.
6. **Real Sheet access.** Still pending - a teammate holds the link. Once available,
   confirm the Apps Script's actual column names match the HTTPS-payload schema this
   plan assumes (`PIPELINE_PLAN.md`, open risk #1) before trusting predictions run
   against the real data.
```

- [ ] **Step 3: Self-review against the spec**

Re-read `docs/superpowers/specs/2026-10-12-q10-shelf-life-prediction-spec.md` section by
section and confirm each requirement maps to a task:
- §1 model formula -> Tasks 2-4 (`rate_factor`, `accumulate_equivalent_hours`,
  `predict_use_by`).
- §2 parameters/sources/traceability -> Task 1 (`config.py`), Task 10 (`SOURCES.md`),
  `PredictionRow`'s traceability fields (Task 1, written in Task 7).
- §3 input schemas/unverified Apps Script -> Tasks 5-6, Global Constraints.
- §4 behaviour (dt capping, implausible filtering, status, MQ-135, output) -> Tasks 3-4,
  7-8.
- §5 structure/constraints (pure functions, tests, CLI, readability) -> all tasks;
  Task 9 for the CLI.
- §6 open questions -> Task 10's `SOURCES.md` "Open questions", carried through from
  this plan's Global Constraints.

- [ ] **Step 4: Commit**

```bash
git add prediction/README.md prediction/SOURCES.md
git commit -m "docs: add prediction package README and IEEE-style source log"
```

## Open Questions (summary)

These are not resolved by this plan - they are documented in `SOURCES.md` (Task 10) and
surfaced at runtime by `pipeline.q10_discrepancy_message()` (Task 8) so they are never
silently decided for the user:

1. Adopt `Q10 = 1.75` (literature-asserted) or `≈1.69` (computed from Mumbre et al.'s
   own reported numbers via Taoukis et al.'s formula)?
2. Is `MQ135_DANGER_RATIO = 5.0` (reused from the firmware) the right MQ-135
   disagreement threshold, given the sensor/food mismatch with Mumbre et al.?
3. The FoodSafety.gov primary-source URL for `L_REF_HOURS` - needs to be retrieved and
   inserted into `SOURCES.md` by the user before submission.
4. Add a separate, stricter `L_REF_HOURS = 24.0` config option now, or wait for real
   trial data?
5. Should `mq135_disagrees` also flag the reverse case (model pessimistic, sensor
   optimistic)?
6. When/how to swap in empirical `Q10`/`L_REF_HOURS` from Stage 1 reference trials once
   that data exists - no task here implements that ingestion.
7. Real Google Sheet access is still pending; confirm its actual columns against the
   HTTPS-payload schema this plan assumes before trusting predictions run against it.
