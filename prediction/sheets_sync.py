"""The only module in this package that talks to the Google Sheets API.

Isolated here so every other module stays testable without live Google
credentials or network access - see
docs/superpowers/specs/2026-10-13-sheets-write-back-design.md section 2.
No pytest tests exist for this module (same spec, section 5); see
prediction/README.md "Manual smoke test" for how to verify it by hand.
"""
from __future__ import annotations

import json
from datetime import date, datetime
from typing import Optional

from google.oauth2 import service_account
from googleapiclient.discovery import build

from prediction.models import PredictionRow

_SCOPES = ["https://www.googleapis.com/auth/spreadsheets"]
_READINGS_RANGE = "Sheet1!A2:E"
_PREDICTIONS_DATE_RANGE = "Predictions!A2:A"
_PREDICTIONS_APPEND_RANGE = "Predictions!A1"
_PREDICTIONS_HEADER_CHECK_RANGE = "Predictions!A1:A1"

# Narrower than data_io.PREDICTION_FIELDS: this is what a human reads in the
# live Sheet, not the full audit trail (that still goes to predictions.csv
# via data_io.write_prediction_row). run_ts stays first and in ISO format -
# read_last_prediction_date's once-per-day gate parses column A with
# datetime.fromisoformat, so it can't be reformatted for display.
_SHEET_PREDICTION_FIELDS = ["run_ts", "predicted_use_by", "status"]


def _sheet_row_values(row: PredictionRow) -> list:
    return [row.run_ts.isoformat(), row.predicted_use_by.strftime("%d/%m/%Y"), row.status]


def load_credentials(key_json: str):
    """Build Sheets API credentials from a service-account JSON key's
    content (the value of the GOOGLE_SERVICE_ACCOUNT_KEY secret), not a
    file path - the key never touches disk in the GitHub Actions runner."""
    info = json.loads(key_json)
    return service_account.Credentials.from_service_account_info(info, scopes=_SCOPES)


def _service(credentials):
    return build("sheets", "v4", credentials=credentials, cache_discovery=False)


def read_readings_rows(spreadsheet_id: str, credentials) -> list[list[str]]:
    result = (
        _service(credentials).spreadsheets().values()
        .get(spreadsheetId=spreadsheet_id, range=_READINGS_RANGE)
        .execute()
    )
    return result.get("values", [])


def read_last_prediction_date(spreadsheet_id: str, credentials) -> Optional[date]:
    result = (
        _service(credentials).spreadsheets().values()
        .get(spreadsheetId=spreadsheet_id, range=_PREDICTIONS_DATE_RANGE)
        .execute()
    )
    values = result.get("values", [])
    if not values:
        return None
    return datetime.fromisoformat(values[-1][0]).date()


def append_prediction_row(spreadsheet_id: str, credentials, row: PredictionRow) -> None:
    sheet = _service(credentials).spreadsheets()
    existing = sheet.values().get(
        spreadsheetId=spreadsheet_id, range=_PREDICTIONS_HEADER_CHECK_RANGE
    ).execute()
    is_new = not existing.get("values")

    body_rows = [_SHEET_PREDICTION_FIELDS] if is_new else []
    body_rows.append(["" if v is None else str(v) for v in _sheet_row_values(row)])

    # RAW, not USER_ENTERED: keeps run_ts stored as the exact ISO string
    # read_last_prediction_date parses back with datetime.fromisoformat - a
    # well-meaning switch to USER_ENTERED would silently break that.
    sheet.values().append(
        spreadsheetId=spreadsheet_id,
        range=_PREDICTIONS_APPEND_RANGE,
        valueInputOption="RAW",
        insertDataOption="INSERT_ROWS",
        body={"values": body_rows},
    ).execute()
