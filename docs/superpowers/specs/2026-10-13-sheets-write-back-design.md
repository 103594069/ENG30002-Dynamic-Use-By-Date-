# Unattended daily Sheet write-back — spec

The Q10 prediction model (`prediction/`) and its CLI are built and tested (see
`2026-10-12-q10-shelf-life-prediction-spec.md`), but nothing currently connects them to
the real Google Sheet on a schedule. This spec covers making that connection fully
unattended: twice a day (8am and 8pm Sydney time), with no dependency on any team
member's laptop being on, and no dependency on the still-blocked Apps Script edit access
(`PIPELINE_PLAN.md` open risk #1). Telegram alerting is explicitly out of scope — a
separate follow-up spec once this is working.

## 1. Chosen approach

GitHub Actions scheduled workflow, authenticating to the Google Sheets API with a
service account, calling the existing (unmodified) Q10 pipeline. Two alternatives were
considered and rejected:

- **Apps Script time-driven trigger calling a hosted Python endpoint** — rejected: still
  blocked on Apps Script edit access (the same open risk #1 that's been stuck since
  before 12 Oct), and adds a *second* new piece of infrastructure (a hosted endpoint) on
  top of the first.
- **Task Scheduler on a team member's own machine** — rejected: not actually unattended.
  Breaks if the machine is off, unplugged, or offline at 8am/8pm; "wake to run" is
  unreliable enough that it's a weak foundation for a multi-day trial meant to run
  hands-off.

GitHub Actions was chosen because it depends on nothing but this repo already being on
GitHub, is free at this scale (two ~1-minute runs a day), and keeps the Q10 model's
single Python implementation as-is (no re-implementation in Apps Script's JS, which the
team explicitly ruled out on 12 Oct).

## 2. Components

Two new modules, isolated so nothing already shipped and tested has to change behaviour:

- **`prediction/sheets_sync.py`** — the *only* module in the whole project that talks to
  the Google Sheets API. Three functions:
  - `read_readings_rows(spreadsheet_id, creds) -> list[list[str]]` — raw rows from the
    Readings tab, header excluded.
  - `read_last_prediction_date(spreadsheet_id, creds) -> Optional[date]` — the date in
    the last row of the Predictions tab, or `None` if the tab is still empty.
  - `append_prediction_row(spreadsheet_id, creds, row: PredictionRow) -> None` — appends
    one row to the Predictions tab (writing the header first if the tab is empty).
- **`prediction/sheets_daily_run.py`** — a new, separate CLI entry point (not a flag
  bolted onto the existing `prediction/cli.py`) that wires `sheets_sync` to the existing
  `pipeline.run_prediction`. Kept separate from `cli.py` because the two have genuinely
  different jobs — "run against a local file, for testing and manual use" vs. "run
  against the network, for the scheduled job" — and conflating them would make the
  argument surface of both harder to reason about.
- **`.github/workflows/daily-prediction.yml`** — cron-triggered twice daily, installs
  dependencies, runs `python -m prediction.sheets_daily_run`.

### Refactor this depends on

`data_io.py`'s `load_sheet_export_csv` currently opens a file and parses it in one
function. It gets split into:
- `_parse_sheet_export_rows(rows: Iterable[list[str]], source_label: str) -> list[Reading]`
  — the existing, already-tested row-parsing/validation logic, now taking an iterable of
  rows instead of a file handle. `source_label` replaces the current error messages'
  file path (e.g. `"Google Sheets API read"` when called from `sheets_daily_run.py`).
- `load_sheet_export_csv(path)` — becomes a thin wrapper: open the file, skip the
  header, hand the rows to `_parse_sheet_export_rows`.

This means `sheets_daily_run.py` calls `_parse_sheet_export_rows` directly on the API's
`values.get` response — the exact same tested parsing and error handling as the file
path, with no parallel implementation and no temporary-file round-trip.

## 3. Data flow

```
cron (8am/8pm Sydney, converted to UTC)
  -> sheets_daily_run.py
    -> authenticate (service account credential)
    -> sheets_sync.read_readings_rows()
    -> _parse_sheet_export_rows()          [same tested parser as the file path]
    -> pipeline.resolve_food_start() + pipeline.run_prediction()   [unchanged]
    -> sheets_sync.read_last_prediction_date()
    -> if that date == today in Australia/Sydney: log "skipped", exit 0
    -> else: sheets_sync.append_prediction_row()
```

`now` and the once-per-day gate's "today" are both computed as an
`Australia/Sydney`-local date via `zoneinfo`, not the GitHub Actions runner's UTC date —
otherwise the gate would drift a day off near local midnight.

## 4. Error handling

Deliberately simple, matching the twice-a-day cadence: any failure (bad or missing
credentials, a malformed row in the Readings tab, no usable readings at all) fails the
GitHub Actions run loudly — exit non-zero, no silent partial write, no retry logic. The
next scheduled run 12 hours later recovers naturally, and the once-per-day gate means a
late/retried run still cannot write a duplicate.

## 5. Testing

Everything except `sheets_sync.py` stays testable exactly as today: fast, offline, no
network, no real credentials. Specifically:
- `_parse_sheet_export_rows` gets the same test cases the current
  `load_sheet_export_csv` tests cover (known fields, ragged row, blank/malformed
  timestamp), now exercised directly against row lists instead of written files.
- `load_sheet_export_csv` keeps a thin test confirming the file-reading wrapper still
  works end to end.
- `sheets_daily_run.py`'s decision logic (skip-if-same-day vs. append, error
  propagation) is unit-tested against a fake `sheets_sync` (dependency injection — the
  real module is never imported by these tests).
- `sheets_sync.py` itself cannot be meaningfully unit-tested without live Google
  credentials. It gets a short, documented manual smoke-test procedure in
  `prediction/README.md` instead, to run once after setup and after any change to that
  module — not part of the `pytest` suite, not part of CI.

## 6. Setup prerequisites (one-time, outside the code)

1. A Google Cloud service account, created in a Google Cloud project under Marcus's own
   account — does not require any teammate's involvement.
2. Its JSON key stored as a GitHub Actions encrypted secret
   (`GOOGLE_SERVICE_ACCOUNT_KEY`) — never committed to the repo.
3. The real Sheet's spreadsheet ID stored as a GitHub Actions repository variable (not a
   secret — it isn't sensitive).
4. The real Sheet shared with the service account's email as **Editor**. This is the one
   remaining dependency on whoever owns the real Sheet — a single share-dialog click,
   much lighter than the still-unresolved Apps Script edit-access ask.
5. New dependencies added to a `requirements.txt` (does not currently exist in the repo):
   `google-api-python-client`, `google-auth`. Scoped to `sheets_sync.py` and
   `sheets_daily_run.py` only — `q10_model.py`, `pipeline.py`, `data_io.py`, and `cli.py`
   stay stdlib-only, matching the 12 Oct decision to keep the core model simple and
   explainable line-by-line for the report.

## 7. Explicitly out of scope

- **Telegram alerting.** Scoped out per 13 Oct discussion — a separate follow-up design
  once this write-back is working, likely from the same `sheets_daily_run.py` trigger
  given it already runs on the right schedule, but not decided here.
- **Writing to the Readings tab.** This design only ever reads it; the device/Apps
  Script side remains the only writer there.
- **Backfilling historical predictions.** The gate only looks at "is there already a row
  for today" — it does not attempt to reconstruct predictions for days the job didn't
  run (e.g. before this is set up).

## 8. Open questions going into planning

1. Exact UTC cron times for 8am/8pm `Australia/Sydney` need to account for daylight
   saving (AEDT vs. AEST) — fine as a fixed offset before 26 Oct, but the workflow file
   should say explicitly which offset it assumes so it isn't a silent landmine later.
2. Whether the service account needs Editor on the whole spreadsheet or can be scoped
   more narrowly (Sheets API permissions are typically whole-file, not per-tab) — needs
   confirming during setup, not assumed here.
