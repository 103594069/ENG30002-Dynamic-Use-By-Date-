from datetime import datetime, timedelta
from pathlib import Path

import pytest

from prediction.data_io import load_https_payload_csv
from prediction.models import Reading
from prediction.pipeline import q10_discrepancy_message, resolve_food_start, run_prediction

# Resolved from this file's own location, not the cwd, so the suite runs
# from the repo root AND from inside prediction/.
MOCK_SHEET = str(Path(__file__).resolve().parent.parent / "mock_data" / "mock_sheet.csv")


def _reading(ts: datetime, temp_c, gas_increase=None) -> Reading:
    return Reading(
        server_ts=ts, device_ts=None, trial="T", source="TEST",
        temp_c=temp_c, humidity_pct=None, pressure_hpa=None, lux=None,
        mq135_raw=None, gas_base=None, gas_increase=gas_increase, risk=None,
    )


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


def test_run_prediction_band_is_ordered_below_reference_temperature():
    """Below T_REF_C (4.0C) a HIGHER Q10 gives a SMALLER rate factor, so it
    spoils more slowly and predicts a LATER use-by - the opposite of the
    above-T_ref case. earliest/latest must therefore be assigned by
    comparing the resulting datetimes, not by assuming which Q10 wins.
    A steady ~2C fridge is the device's primary real-world condition."""
    now = datetime(2026, 1, 1, 6, 0)
    readings = [
        _reading(now - timedelta(hours=6), 2.0),
        _reading(now - timedelta(hours=4), 2.0),
        _reading(now - timedelta(hours=2), 2.0),
        _reading(now, 2.0),
    ]

    row = run_prediction(readings, now)

    assert row.predicted_use_by_earliest <= row.predicted_use_by <= row.predicted_use_by_latest
    # Sanity-check the direction itself: at 2C the q10=1.5 end is the
    # earliest and the q10=2.0 end is the latest, so the band is not
    # degenerate (both ends equal) and really did get swapped.
    assert row.predicted_use_by_earliest < row.predicted_use_by_latest


def test_run_prediction_rejects_empty_readings():
    with pytest.raises(ValueError, match="No usable readings"):
        run_prediction([], datetime(2026, 1, 1, 12, 0))


def test_run_prediction_rejects_readings_that_are_all_implausible():
    now = datetime(2026, 1, 1, 12, 0)
    readings = [
        _reading(now - timedelta(hours=2), None),
        _reading(now - timedelta(hours=1), 999.0),
        _reading(now, None),
    ]
    with pytest.raises(ValueError, match="No usable readings"):
        run_prediction(readings, now)


def test_run_prediction_reads_mq135_from_the_unfiltered_readings():
    """MQ-135 is an independent indicator, so a dropped temperature on the
    newest sample must not silently substitute an older gas reading."""
    now = datetime(2026, 1, 1, 6, 0)
    readings = [
        _reading(now - timedelta(hours=4), 2.0, gas_increase=1.0),
        _reading(now - timedelta(hours=2), 2.0, gas_increase=1.1),
        _reading(now, 999.0, gas_increase=7.5),  # bad temp, good gas reading
    ]

    row = run_prediction(readings, now)

    assert row.dropped_reading_count == 1
    assert row.mq135_gas_increase == 7.5
    assert row.mq135_disagrees is True  # Fresh, but gas is over the 5.0 ratio


def test_q10_discrepancy_message_names_both_q10_values():
    message = q10_discrepancy_message()
    assert message is not None
    assert "config.Q10=1.75" in message
    assert "Q10=1.687" in message  # derive_q10_from_ratio(3.0, 21.0), 3dp
    assert "SOURCES.md" in message
