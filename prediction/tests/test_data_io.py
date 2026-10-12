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
