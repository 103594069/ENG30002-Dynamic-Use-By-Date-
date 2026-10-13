from datetime import date, datetime
from zoneinfo import ZoneInfo

import pytest

from prediction import sheets_daily_run

SYDNEY = ZoneInfo("Australia/Sydney")

# Real-schema rows (Timestamp, MQ135, Temp, Humidity, Pressure), the same
# shape sheets_sync.read_readings_rows returns.
ROWS = [
    ["10/6/2026 17:52:57", "133", "18.72", "52.85", "1014.12"],
    ["10/6/2026 17:55:39", "112", "19.13", "51.23", "1014.17"],
]


class _FakeSync:
    def __init__(self, rows, last_date):
        self.rows = rows
        self.last_date = last_date
        self.appended = []

    def read_readings_rows(self, spreadsheet_id, credentials):
        return self.rows

    def read_last_prediction_date(self, spreadsheet_id, credentials):
        return self.last_date

    def append_prediction_row(self, spreadsheet_id, credentials, row):
        self.appended.append(row)


def test_run_writes_a_new_prediction_and_returns_true():
    fake = _FakeSync(rows=ROWS, last_date=date(2026, 10, 5))
    now = datetime(2026, 10, 6, 20, 0, 0, tzinfo=SYDNEY)

    result = sheets_daily_run.run("sheet123", "fake-creds", sync=fake, now=now)

    assert result is True
    assert len(fake.appended) == 1
    assert fake.appended[0].status == "Fresh"


def test_run_skips_when_todays_prediction_is_already_recorded(capsys):
    fake = _FakeSync(rows=ROWS, last_date=date(2026, 10, 6))
    now = datetime(2026, 10, 6, 20, 0, 0, tzinfo=SYDNEY)

    result = sheets_daily_run.run("sheet123", "fake-creds", sync=fake, now=now)

    assert result is False
    assert fake.appended == []
    assert "Skipped: a prediction for 2026-10-06 was already recorded." in capsys.readouterr().out


def test_run_propagates_a_malformed_reading_error():
    fake = _FakeSync(rows=[["not-a-timestamp", "1", "2", "3", "4"]], last_date=None)
    now = datetime(2026, 10, 6, 20, 0, 0, tzinfo=SYDNEY)

    with pytest.raises(ValueError, match="row 2"):
        sheets_daily_run.run("sheet123", "fake-creds", sync=fake, now=now)


def test_run_uses_sydney_date_not_utc_date_for_the_gate():
    # 9am Sydney on Oct 6 is 10pm UTC on Oct 5 - if the gate ever used the UTC date
    # instead of the Sydney date, this would wrongly match last_date and skip.
    now = datetime(2026, 10, 6, 9, 0, 0, tzinfo=SYDNEY)
    fake = _FakeSync(rows=ROWS, last_date=date(2026, 10, 5))

    result = sheets_daily_run.run("sheet123", "fake-creds", sync=fake, now=now)

    assert result is True
    assert fake.appended[0].run_ts.date() == date(2026, 10, 6)
