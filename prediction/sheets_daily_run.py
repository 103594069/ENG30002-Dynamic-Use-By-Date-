"""Entry point for the scheduled (GitHub Actions) daily Sheet write-back.

Separate from cli.py: that module reads a local file for manual/testing
use; this one reads the live Sheet on a schedule. See
docs/superpowers/specs/2026-10-13-sheets-write-back-design.md.
"""
from __future__ import annotations

import os
from datetime import datetime
from typing import Optional
from zoneinfo import ZoneInfo

from prediction import config, sheets_sync
from prediction.data_io import _parse_sheet_export_rows
from prediction.pipeline import q10_discrepancy_message, resolve_food_start, run_prediction

_SYDNEY = ZoneInfo("Australia/Sydney")


def run(spreadsheet_id: str, credentials, sync=sheets_sync, now: Optional[datetime] = None) -> bool:
    """Runs one prediction cycle against the live Sheet.

    `sync` defaults to the real sheets_sync module; tests inject a fake
    with the same three-method shape so this runs with no network
    access at all. `now`, if given, must be timezone-aware (tests pass
    an Australia/Sydney-aware datetime); if omitted, the current
    Australia/Sydney time is used. Returns True if a row was written,
    False if skipped because today's prediction was already recorded.
    """
    now = now or datetime.now(_SYDNEY)
    today = now.date()

    last_date = sync.read_last_prediction_date(spreadsheet_id, credentials)
    if last_date == today:
        print(f"Skipped: a prediction for {today} was already recorded.")
        return False

    warning = q10_discrepancy_message()
    if warning:
        print(warning)

    rows = sync.read_readings_rows(spreadsheet_id, credentials)
    readings = _parse_sheet_export_rows(rows, "Google Sheets API read")

    # The pipeline's internals (resolve_food_start, run_prediction) compare
    # against Reading.server_ts, which is always naive - so `now` must be
    # naive here too, even though it was timezone-aware above for the date
    # comparison. See this task's note in the implementation plan.
    now_naive = now.replace(tzinfo=None)

    start = resolve_food_start(readings, config.FOOD_START_TS, trial=None)
    readings = sorted((r for r in readings if r.server_ts >= start), key=lambda r: r.server_ts)

    row = run_prediction(readings, now_naive)
    sync.append_prediction_row(spreadsheet_id, credentials, row)

    print(f"Status: {row.status}")
    print(f"Predicted use-by: {row.predicted_use_by.isoformat()}")
    return True


def main() -> None:
    key_json = os.environ["GOOGLE_SERVICE_ACCOUNT_KEY"]
    spreadsheet_id = os.environ["SPREADSHEET_ID"]
    credentials = sheets_sync.load_credentials(key_json)
    run(spreadsheet_id, credentials)


if __name__ == "__main__":
    main()
