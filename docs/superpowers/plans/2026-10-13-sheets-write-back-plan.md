# Unattended Daily Sheet Write-Back Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Q10 prediction run unattended, twice a day (8am/8pm Sydney time), writing one row per day to the real Sheet's Predictions tab via GitHub Actions and the Google Sheets API.

**Architecture:** A new isolated module (`sheets_sync.py`) owns all Google Sheets API calls; a new entry point (`sheets_daily_run.py`) wires it to the existing, unmodified Q10 pipeline; a GitHub Actions cron workflow runs it twice daily. The existing file-based CLI, loaders, and Q10 math are untouched except for one small refactor that lets the new code reuse the existing tested CSV-row parser without a temp-file round-trip.

**Tech Stack:** Python (stdlib + `google-api-python-client`/`google-auth`, scoped to the two new modules only), pytest, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-13-sheets-write-back-design.md`

## Global Constraints

- `q10_model.py`, `pipeline.py`, `config.py`, and `cli.py` stay unmodified and stdlib-only — the two new Google API dependencies are confined to `sheets_sync.py` and `sheets_daily_run.py` (spec §6).
- All existing tests must stay green throughout every task (37 passing as of this plan: `python -m pytest prediction/tests -v`).
- `sheets_sync.py` gets no pytest tests — it's the one module that can't be meaningfully tested without live Google credentials. It gets a documented manual smoke-test procedure instead (spec §5). Do not attempt to mock the Google API client to force unit coverage here.
- The once-per-day gate's "today" is always computed in `Australia/Sydney` local time via `zoneinfo`, never the GitHub Actions runner's UTC date (spec §3).
- No retry/backoff logic anywhere in this feature — failures propagate and fail the job loudly; the next scheduled run 12 hours later recovers naturally (spec §4).
- Telegram alerting, writing to the Readings tab, and backfilling historical predictions are explicitly out of scope (spec §7) — do not add them even as a convenience.

---

### Task 1: Extract a reusable row-parser out of `load_sheet_export_csv`

**Files:**
- Modify: `prediction/data_io.py` (currently 268 lines — see lines 157–197 for `load_sheet_export_csv`, lines 200–231 for `_PREDICTION_FIELDS`/`_prediction_row_values`)
- Test: `prediction/tests/test_data_io.py`

**Interfaces:**
- Produces: `prediction.data_io._parse_sheet_export_rows(rows: Iterable[list[str]], source_label: str) -> list[Reading]` — pure parsing/validation logic, header already excluded from `rows`. Raises `ValueError` naming `source_label`, the row number, and the offending value, exactly like the current file-based error messages do.
- Produces: `prediction.data_io.load_sheet_export_csv(path: str) -> list[Reading]` — unchanged signature and behavior, now a thin wrapper calling the function above with `source_label=path`.
- Produces: `prediction.data_io.PREDICTION_FIELDS` (renamed from `_PREDICTION_FIELDS`) and `prediction.data_io.prediction_row_values` (renamed from `_prediction_row_values`) — made public because Task 2's `sheets_sync.py` needs to reuse them to keep the real Sheet's Predictions tab in the exact same column shape as the local `predictions.csv`.
- Consumes: nothing new — this task only rearranges existing, already-tested code.

- [ ] **Step 1: Write the failing tests**

Add to `prediction/tests/test_data_io.py`, alongside the existing `load_sheet_export_csv` tests:

```python
from prediction.data_io import _parse_sheet_export_rows  # add to the existing import block


def test_parse_sheet_export_rows_parses_known_fields_by_position():
    rows = [["10/6/2026 17:52:57", "133", "18.72", "52.85", "1014.12"]]

    readings = _parse_sheet_export_rows(rows, "Google Sheets API read")

    assert len(readings) == 1
    r = readings[0]
    assert r.server_ts == datetime(2026, 10, 6, 17, 52, 57)
    assert r.mq135_raw == 133
    assert r.temp_c == 18.72
    assert r.humidity_pct == 52.85
    assert r.pressure_hpa == 1014.12


def test_parse_sheet_export_rows_rejects_a_malformed_timestamp_with_the_given_label():
    rows = [["2026-10-06T17:52:57", "133", "18.72", "52.85", "1014.12"]]

    with pytest.raises(ValueError) as excinfo:
        _parse_sheet_export_rows(rows, "Google Sheets API read")

    message = str(excinfo.value)
    assert "Google Sheets API read" in message
    assert "row 2" in message
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest prediction/tests/test_data_io.py -v -k parse_sheet_export_rows`
Expected: FAIL with `ImportError: cannot import name '_parse_sheet_export_rows'`

- [ ] **Step 3: Extract the function and rename the two prediction-row helpers**

In `prediction/data_io.py`, replace the body of `load_sheet_export_csv` (lines 157–197) with:

```python
def _parse_sheet_export_rows(rows, source_label: str) -> list[Reading]:
    """Parse rows already split out of the real Sheet's export schema.

    `rows` excludes the header; `source_label` replaces the file path in
    error messages (e.g. "Google Sheets API read" when called from
    sheets_daily_run.py instead of a local file) - see
    load_sheet_export_csv's docstring for the schema this expects.
    """
    readings: list[Reading] = []
    for row_number, row in enumerate(rows, start=2):
        raw_ts = _row_value(row, 0)
        server_ts = _parse_sheet_export_ts(raw_ts)
        if server_ts is None:
            raise ValueError(
                f"{source_label}: row {row_number} has an invalid or missing "
                f"Timestamp: {raw_ts!r}"
            )
        readings.append(Reading(
            server_ts=server_ts,
            device_ts=None,
            trial=None,
            source=None,
            temp_c=_parse_float(_row_value(row, 2)),
            humidity_pct=_parse_float(_row_value(row, 3)),
            pressure_hpa=_parse_float(_row_value(row, 4)),
            lux=None,
            mq135_raw=_parse_float(_row_value(row, 1)),
            gas_base=None,
            gas_increase=None,
            risk=None,
        ))
    return readings


def load_sheet_export_csv(path: str) -> list[Reading]:
    """Load readings from the real Google Sheet's CSV export.

    Expected column ORDER - matched by position, not name, because the
    real export's 5th column has no header text at all: Timestamp,
    MQ135, Temp, Humidity, Pressure. Timestamp is Google Sheets' US
    locale format (M/D/YYYY H:MM:SS), not ISO8601. This schema has no
    trial/source/device_ts/lux/gas_base/gas_increase/risk columns, so
    those fields are always None - narrower than load_https_payload_csv.

    Raises ValueError naming the file, row, and offending value if
    Timestamp is missing, blank, or unparseable, for the same reason
    load_https_payload_csv guards server_ts.
    """
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        return _parse_sheet_export_rows(reader, path)
```

Then rename every use of `_PREDICTION_FIELDS` to `PREDICTION_FIELDS` and `_prediction_row_values` to `prediction_row_values` (the definitions at lines 200–231, and their two call sites in `write_prediction_row` and `append_daily_prediction`).

- [ ] **Step 4: Run the full test suite to verify everything passes**

Run: `python -m pytest prediction/tests -v`
Expected: PASS, 39 passed (37 existing + 2 new from Step 1) — no existing test should need any edit, since `load_sheet_export_csv`'s observable behavior (including its error messages, which still embed the file path as `source_label`) is unchanged.

- [ ] **Step 5: Commit**

```bash
git add prediction/data_io.py prediction/tests/test_data_io.py
git commit -m "refactor: extract _parse_sheet_export_rows for reuse by the Sheets API path"
```

---

### Task 2: `sheets_sync.py` — the only module that talks to the Google Sheets API

**Files:**
- Create: `prediction/sheets_sync.py`

**Interfaces:**
- Consumes: `prediction.data_io.PREDICTION_FIELDS`, `prediction.data_io.prediction_row_values` (Task 1), `prediction.models.PredictionRow`.
- Produces:
  - `load_credentials(key_json: str)` — builds a `google.oauth2.service_account.Credentials` from a JSON key given as a string (so callers pass the *content* of a secret, never a file path that might get committed).
  - `read_readings_rows(spreadsheet_id: str, credentials) -> list[list[str]]` — raw rows from the Readings tab, `A2:E` (header excluded).
  - `read_last_prediction_date(spreadsheet_id: str, credentials) -> Optional[datetime.date]` — the date parsed from the last row's `run_ts` column of the Predictions tab, or `None` if that tab has no data rows yet.
  - `append_prediction_row(spreadsheet_id: str, credentials, row: PredictionRow) -> None` — appends one row to the Predictions tab, writing `PREDICTION_FIELDS` as a header first if the tab is empty.

This task has **no pytest tests** — see Global Constraints. Verification is a syntax/import check now and a manual smoke test in Task 5, once real credentials exist.

- [ ] **Step 1: Write the module**

Create `prediction/sheets_sync.py`:

```python
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

from prediction.data_io import PREDICTION_FIELDS, prediction_row_values
from prediction.models import PredictionRow

_SCOPES = ["https://www.googleapis.com/auth/spreadsheets"]
_READINGS_RANGE = "Readings!A2:E"
_PREDICTIONS_DATE_RANGE = "Predictions!A2:A"
_PREDICTIONS_APPEND_RANGE = "Predictions!A1"
_PREDICTIONS_HEADER_CHECK_RANGE = "Predictions!A1:A1"


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

    body_rows = [PREDICTION_FIELDS] if is_new else []
    body_rows.append([str(v) for v in prediction_row_values(row)])

    sheet.values().append(
        spreadsheetId=spreadsheet_id,
        range=_PREDICTIONS_APPEND_RANGE,
        valueInputOption="RAW",
        insertDataOption="INSERT_ROWS",
        body={"values": body_rows},
    ).execute()
```

- [ ] **Step 2: Add the two new dependencies so the import check can run**

This module needs `google-api-python-client` and `google-auth`, which Task 4 formalizes into `requirements.txt`. Install them now so Step 3 below can run:

Run: `pip install google-api-python-client google-auth`

- [ ] **Step 3: Verify the module imports cleanly**

Run: `python -c "import prediction.sheets_sync"`
Expected: no output, exit code 0. This only catches syntax/import errors — it does not call the network (the Sheets API client is built lazily inside each function, never at import time).

- [ ] **Step 4: Run the full test suite to confirm nothing else broke**

Run: `python -m pytest prediction/tests -v`
Expected: PASS, same count as the end of Task 1 (this task added no tests).

- [ ] **Step 5: Commit**

```bash
git add prediction/sheets_sync.py
git commit -m "feat: add sheets_sync.py, the only module that calls the Google Sheets API"
```

---

### Task 3: `sheets_daily_run.py` — orchestration entry point

**Files:**
- Create: `prediction/sheets_daily_run.py`
- Test: `prediction/tests/test_sheets_daily_run.py`

**Interfaces:**
- Consumes: `prediction.sheets_sync` (Task 2, used as the default `sync` dependency — tests inject a fake with the same three-method shape instead), `prediction.data_io._parse_sheet_export_rows` (Task 1), `prediction.pipeline.{q10_discrepancy_message, resolve_food_start, run_prediction}` (unchanged), `prediction.config` (unchanged).
- Produces: `prediction.sheets_daily_run.run(spreadsheet_id: str, credentials, sync=sheets_sync, now: Optional[datetime] = None) -> bool` — returns `True` if a row was written, `False` if skipped as a same-day duplicate. `prediction.sheets_daily_run.main() -> None` — reads `GOOGLE_SERVICE_ACCOUNT_KEY` and `SPREADSHEET_ID` from the environment and calls `run`.

**Important implementation detail (read before writing the code):** `datetime.now(ZoneInfo("Australia/Sydney"))` returns a *timezone-aware* datetime, but every `Reading.server_ts` produced by the loaders is *naive* (no tzinfo — see `_parse_sheet_export_ts` in `data_io.py`). Comparing an aware and a naive datetime (which `pipeline.run_prediction` and `pipeline.resolve_food_start` both do internally) raises `TypeError`. `run()` must therefore compute `now` as timezone-aware only long enough to take its `.date()` for the gate check, then strip the tzinfo (`.replace(tzinfo=None)`) before passing it into the pipeline — exactly as shown in Step 3 below. This mirrors how `cli.py`'s `--now` is already a naive datetime.

- [ ] **Step 1: Write the failing tests**

Create `prediction/tests/test_sheets_daily_run.py`:

```python
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest prediction/tests/test_sheets_daily_run.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'prediction.sheets_daily_run'`

- [ ] **Step 3: Write the implementation**

Create `prediction/sheets_daily_run.py`:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest prediction/tests/test_sheets_daily_run.py -v`
Expected: PASS, 3 passed

- [ ] **Step 5: Run the full test suite**

Run: `python -m pytest prediction/tests -v`
Expected: PASS, same count as Task 2's end plus 3 (no other file should need changes)

- [ ] **Step 6: Commit**

```bash
git add prediction/sheets_daily_run.py prediction/tests/test_sheets_daily_run.py
git commit -m "feat: add sheets_daily_run.py orchestration entry point"
```

---

### Task 4: `requirements.txt` and the GitHub Actions workflow

**Files:**
- Create: `requirements.txt`
- Create: `.github/workflows/daily-prediction.yml`

**Interfaces:**
- Consumes: `prediction.sheets_daily_run.main()` (Task 3), via `python -m prediction.sheets_daily_run`.
- Produces: nothing other modules import — this is the operational trigger, not a library.

- [ ] **Step 1: Write `requirements.txt`**

```
google-api-python-client>=2.0.0
google-auth>=2.0.0
```

- [ ] **Step 2: Write the workflow file**

Create `.github/workflows/daily-prediction.yml`:

```yaml
name: Daily use-by prediction

on:
  schedule:
    # 8am and 8pm Australia/Sydney. As of Oct 2026, Sydney is on AEDT
    # (UTC+11, daylight saving), so 8am local = 21:00 UTC the previous
    # day and 8pm local = 09:00 UTC. If this workflow is still running
    # after the switch back to AEST (UTC+10, typically early April),
    # both times below need to shift one hour LATER in UTC to keep
    # firing at 8am/8pm local.
    - cron: '0 21 * * *'  # 8am AEDT
    - cron: '0 9 * * *'   # 8pm AEDT
  workflow_dispatch: {}   # allows a manual run from the Actions tab, for the smoke test in Task 5

jobs:
  predict:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.11'
      - run: pip install -r requirements.txt
      - run: python -m prediction.sheets_daily_run
        env:
          GOOGLE_SERVICE_ACCOUNT_KEY: ${{ secrets.GOOGLE_SERVICE_ACCOUNT_KEY }}
          SPREADSHEET_ID: ${{ vars.SPREADSHEET_ID }}
```

- [ ] **Step 3: Validate the YAML parses**

Run: `python -c "import yaml, sys; yaml.safe_load(open('.github/workflows/daily-prediction.yml', encoding='utf-8')) and print('OK')"`

If this fails with `ModuleNotFoundError: No module named 'yaml'`, run `pip install pyyaml` first — it's a one-off check, not a project dependency, so do not add it to `requirements.txt`.
Expected: prints `OK`

- [ ] **Step 4: Commit**

```bash
git add requirements.txt .github/workflows/daily-prediction.yml
git commit -m "ci: add requirements.txt and the daily-prediction GitHub Actions workflow"
```

(No change to `prediction/tests` in this task, so no need to re-run the suite — nothing it covers changed.)

---

### Task 5: Setup instructions and manual smoke-test procedure in `prediction/README.md`

**Files:**
- Modify: `prediction/README.md`

**Interfaces:**
- Consumes: nothing new — this is documentation of the feature built in Tasks 1–4.
- Produces: nothing other code imports — this is the human-facing setup/verification guide.

- [ ] **Step 1: Add a new section**

Add this section to `prediction/README.md`, after the existing "### The real Sheet" subsection:

```markdown
### Unattended daily write-back (GitHub Actions)

`sheets_daily_run.py` + `.github/workflows/daily-prediction.yml` run the prediction
automatically at 8am and 8pm Sydney time, writing one row per day to the real Sheet's
"Predictions" tab via the Google Sheets API. See
`docs/superpowers/specs/2026-10-13-sheets-write-back-design.md` for the full design.

**One-time setup, before the schedule will work:**

1. In Google Cloud Console, create a project (or reuse one), enable the "Google Sheets
   API", and create a service account. Download its JSON key.
2. In the real Sheet, click Share and add the service account's email (ends in
   `...gserviceaccount.com`, found on its Google Cloud Console page) as an **Editor**.
   (The Sheets API only supports whole-file sharing, not per-tab — there's no way to
   grant it write access to Predictions without also granting it Readings, even though
   it only ever reads Readings. Flagged as an open question in the design spec; Editor
   on the whole file is the only option, so there's nothing to decide here in practice.)
3. In this repo's GitHub settings: Settings -> Secrets and variables -> Actions ->
   New repository secret, name `GOOGLE_SERVICE_ACCOUNT_KEY`, paste the entire contents
   of the downloaded JSON key file.
4. Same page, "Variables" tab -> New repository variable, name `SPREADSHEET_ID`, value
   is the real Sheet's ID (the long string in its URL between `/d/` and `/edit`).
5. Confirm the Sheet has a tab literally named `Readings` (matching the schema in "The
   real Sheet" above) and a tab literally named `Predictions` (can start empty -
   `append_prediction_row` writes the header on its first run).

**Manual smoke test**, to run once after setup and after any change to
`sheets_sync.py` (this module has no automated tests - see the design spec, section 5):

1. Go to this repo's Actions tab -> "Daily use-by prediction" -> "Run workflow" (this
   works because of `workflow_dispatch` in the workflow file - no need to wait for
   8am/8pm).
2. Check the run's log: it should either print `Status: ...` and `Predicted use-by:
   ...` (a row was written) or `Skipped: a prediction for <today> was already
   recorded.` (if you've already run it today).
3. Open the real Sheet's Predictions tab and confirm a new row appeared with today's
   date in its first column, matching the log output.
4. Run the workflow a second time the same day and confirm it logs `Skipped: ...` and
   does **not** add a second row - this is the once-per-day gate working.
```

- [ ] **Step 2: Commit**

```bash
git add prediction/README.md
git commit -m "docs: add setup instructions and manual smoke test for the Sheet write-back"
```

---

### Task 6: Update `PIPELINE_PLAN.md`

**Files:**
- Modify: `PIPELINE_PLAN.md`

**Interfaces:** none — documentation only.

- [ ] **Step 1: Add a dated entry**

Add this as a new subsection directly under the existing "## What changed 13 Oct" heading (above its first existing bullet):

```markdown
- **Unattended daily Sheet write-back implemented** (code complete, not yet activated).
  `prediction/sheets_daily_run.py` + `.github/workflows/daily-prediction.yml` run the
  prediction at 8am/8pm Sydney time via GitHub Actions and the Google Sheets API,
  writing one row per day to the real Sheet's Predictions tab - see
  `docs/superpowers/specs/2026-10-13-sheets-write-back-design.md` for the design and
  `prediction/README.md` "Unattended daily write-back" for setup. **Not live yet**:
  needs the one-time Google Cloud service-account setup and the real Sheet's owner to
  grant that service account Editor access (`prediction/README.md` step 2) before the
  schedule will actually write anything. Open risk #7 (Telegram integration shape) is
  unaffected - still open, explicitly out of scope for this write-back.
```

- [ ] **Step 2: Commit**

```bash
git add PIPELINE_PLAN.md
git commit -m "docs: note the Sheet write-back is implemented but not yet activated"
```
