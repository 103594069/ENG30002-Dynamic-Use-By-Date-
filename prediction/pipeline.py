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
