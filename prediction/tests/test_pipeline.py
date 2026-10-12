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
