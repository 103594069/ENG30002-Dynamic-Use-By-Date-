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
