from datetime import datetime

from prediction.data_io import load_https_payload_csv, load_sd_datacsv, write_prediction_row
from prediction.models import PredictionRow

HTTPS_CSV = """server_ts,device_ts,trial,source,temp,humidity,pressure,lux,mq135,gas_base,gas_increase,risk
2026-10-01T09:00:00,2026-10-01T09:00:01,ChickenTrial1,MANUAL,4.0,55.0,1015.0,0.0,900,900,1.0,SAFE
2026-10-01T09:30:00,,ChickenTrial1,SCHEDULED,,55.0,1015.0,0.0,910,900,1.011,SAFE
"""

SD_CSV = """date,time,trial,temp_C,humidity_pct,pressure_hPa,lux,mq135_raw,mq135_base,mq135_increase,source
2026-10-01,09:00:00,ChickenTrial1,4.00,55.00,1015.00,0.0,900,900,1.000,MANUAL
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
