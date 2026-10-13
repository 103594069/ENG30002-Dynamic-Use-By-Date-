from datetime import datetime

import pytest

from prediction.data_io import (
    append_daily_prediction,
    load_https_payload_csv,
    load_sd_datacsv,
    load_sheet_export_csv,
    write_prediction_row,
)
from prediction.models import PredictionRow

HTTPS_CSV = """server_ts,device_ts,trial,source,temp,humidity,pressure,lux,mq135,gas_base,gas_increase,risk
2026-10-01T09:00:00,2026-10-01T09:00:01,ChickenTrial1,MANUAL,4.0,55.0,1015.0,0.0,900,900,1.0,SAFE
2026-10-01T09:30:00,,ChickenTrial1,SCHEDULED,,55.0,1015.0,0.0,910,900,1.011,SAFE
"""

SD_CSV = """date,time,trial,temp_C,humidity_pct,pressure_hPa,lux,mq135_raw,mq135_base,mq135_increase,source
2026-10-01,09:00:00,ChickenTrial1,4.00,55.00,1015.00,0.0,900,900,1.000,MANUAL
"""

# Matches the real Sheet's actual CSV export: Timestamp, MQ135, Temp,
# Humidity, then a 5th column with no header text at all (pressure).
# Timestamps are Google Sheets' US locale format, not ISO8601.
SHEET_EXPORT_CSV = """Timestamp,MQ135,Temp,Humidity,
10/6/2026 17:52:57,133,18.72,52.85,1014.12
10/6/2026 17:55:39,112,19.13,51.23,1014.17
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


def test_load_https_payload_csv_tolerates_a_ragged_row(tmp_path):
    """csv.DictReader fills missing trailing fields with None, not "" -
    a row with fewer columns than the header must still load."""
    path = tmp_path / "ragged.csv"
    path.write_text(
        "server_ts,device_ts,trial,source,temp,humidity,pressure,lux,"
        "mq135,gas_base,gas_increase,risk\n"
        "2026-10-01T09:00:00,2026-10-01T09:00:01,ChickenTrial1,MANUAL,4.0\n",
        encoding="utf-8",
    )

    readings = load_https_payload_csv(str(path))

    assert len(readings) == 1
    r = readings[0]
    assert r.server_ts == datetime(2026, 10, 1, 9, 0, 0)
    assert r.temp_c == 4.0
    assert r.humidity_pct is None
    assert r.gas_increase is None
    assert r.risk is None


def test_load_sd_datacsv_tolerates_a_ragged_row(tmp_path):
    path = tmp_path / "ragged_data.csv"
    path.write_text(
        "date,time,trial,temp_C,humidity_pct,pressure_hPa,lux,"
        "mq135_raw,mq135_base,mq135_increase,source\n"
        "2026-10-01,09:00:00,ChickenTrial1,4.00\n",
        encoding="utf-8",
    )

    readings = load_sd_datacsv(str(path))

    assert len(readings) == 1
    r = readings[0]
    assert r.server_ts == datetime(2026, 10, 1, 9, 0, 0)
    assert r.temp_c == 4.0
    assert r.humidity_pct is None
    assert r.gas_increase is None
    assert r.source is None


def test_load_https_payload_csv_rejects_a_blank_server_ts(tmp_path):
    path = tmp_path / "blank_ts.csv"
    path.write_text(
        "server_ts,device_ts,trial,source,temp\n"
        "2026-10-01T09:00:00,,ChickenTrial1,MANUAL,4.0\n"
        ",,ChickenTrial1,MANUAL,4.1\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError) as excinfo:
        load_https_payload_csv(str(path))

    message = str(excinfo.value)
    assert "row 3" in message            # header is row 1, so the bad row is 3
    assert "server_ts" in message
    assert "blank_ts.csv" in message


def test_load_https_payload_csv_rejects_a_malformed_server_ts(tmp_path):
    path = tmp_path / "bad_ts.csv"
    path.write_text(
        "server_ts,device_ts,trial,source,temp\n"
        "not-a-timestamp,,ChickenTrial1,MANUAL,4.0\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="row 2"):
        load_https_payload_csv(str(path))


def test_load_sd_datacsv_rejects_a_malformed_date_or_time(tmp_path):
    path = tmp_path / "bad_data.csv"
    path.write_text(
        "date,time,trial,temp_C\n"
        "2026-10-01,09:00:00,ChickenTrial1,4.00\n"
        "2026-13-99,not-a-time,ChickenTrial1,4.00\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError) as excinfo:
        load_sd_datacsv(str(path))

    message = str(excinfo.value)
    assert "row 3" in message
    assert "bad_data.csv" in message
    assert "not-a-time" in message


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


def test_load_sheet_export_csv_parses_known_fields_by_position(tmp_path):
    path = tmp_path / "export.csv"
    path.write_text(SHEET_EXPORT_CSV, encoding="utf-8")

    readings = load_sheet_export_csv(str(path))

    assert len(readings) == 2
    first = readings[0]
    assert first.server_ts == datetime(2026, 10, 6, 17, 52, 57)
    assert first.mq135_raw == 133
    assert first.temp_c == 18.72
    assert first.humidity_pct == 52.85
    assert first.pressure_hpa == 1014.12
    # Fields the real export has no column for at all:
    assert first.trial is None
    assert first.source is None
    assert first.gas_increase is None


def test_load_sheet_export_csv_tolerates_a_ragged_row(tmp_path):
    path = tmp_path / "ragged_export.csv"
    path.write_text(
        "Timestamp,MQ135,Temp,Humidity,\n"
        "10/6/2026 17:52:57,133,18.72\n",
        encoding="utf-8",
    )

    readings = load_sheet_export_csv(str(path))

    assert len(readings) == 1
    r = readings[0]
    assert r.temp_c == 18.72
    assert r.humidity_pct is None
    assert r.pressure_hpa is None


def test_load_sheet_export_csv_rejects_a_blank_timestamp(tmp_path):
    path = tmp_path / "blank_export.csv"
    path.write_text(
        "Timestamp,MQ135,Temp,Humidity,\n"
        "10/6/2026 17:52:57,133,18.72,52.85,1014.12\n"
        ",112,19.13,51.23,1014.17\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError) as excinfo:
        load_sheet_export_csv(str(path))

    message = str(excinfo.value)
    assert "row 3" in message
    assert "Timestamp" in message
    assert "blank_export.csv" in message


def test_load_sheet_export_csv_rejects_a_malformed_timestamp(tmp_path):
    path = tmp_path / "bad_export.csv"
    path.write_text(
        "Timestamp,MQ135,Temp,Humidity,\n"
        "2026-10-06T17:52:57,133,18.72,52.85,1014.12\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="row 2"):
        load_sheet_export_csv(str(path))


def test_append_daily_prediction_writes_first_row_then_skips_same_day(tmp_path):
    path = tmp_path / "daily_predictions.csv"
    row = _sample_row()  # run_ts = 2026-10-02T09:00:00

    first_write = append_daily_prediction(str(path), row)
    second_write = append_daily_prediction(str(path), row)  # same calendar day

    assert first_write is True
    assert second_write is False
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 2  # header + one row, second call was a no-op


def test_append_daily_prediction_writes_a_new_row_for_a_new_day(tmp_path):
    path = tmp_path / "daily_predictions.csv"
    day1 = _sample_row()  # run_ts = 2026-10-02T09:00:00
    day2 = _sample_row()
    day2.run_ts = datetime(2026, 10, 3, 9, 0, 0)

    append_daily_prediction(str(path), day1)
    second_write = append_daily_prediction(str(path), day2)

    assert second_write is True
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 3  # header + two days
