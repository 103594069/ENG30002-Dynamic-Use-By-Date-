# Q10 shelf-life prediction

Predicts a use-by date for raw chicken from logged ESP32 sensor readings, using a
literature-derived Q10 temperature-acceleration model. See `SOURCES.md` for every
parameter's source, justification, and open questions.

## Usage

    pip install pytest          # only needed to run the tests
    python -m prediction.cli --readings path/to/readings.csv --trial ChickenTrial1

By default this reads the HTTPS-payload schema (what `food_monitor_compact.ino`'s
`publishToGoogleSheets()` sends). Pass `--format datacsv` to read the on-device
`DATA.CSV` schema instead, or `--format sheetexport` for the real Sheet's actual
CSV export schema (see "The real Sheet" below). Results are appended to
`predictions.csv` (override with `--predictions-out`).

Add `--once-per-day` to gate the write: if `--predictions-out` already has a row for
today's date, the run prints `Skipped: ...` instead of appending a duplicate. This
models a daily trigger that might fire more than once a day without double-counting.

### The real Sheet

As of 13 Oct the real Sheet is visible (link shared) and its actual CSV export
columns are narrower than the HTTPS-payload schema assumed above: `Timestamp, MQ135,
Temp, Humidity`, plus a 5th column with **no header text at all** (pressure). No
`trial`, `source`, `device_ts`, `lux`, `gas_base`, `gas_increase`, or `risk` columns
exist. Use `--format sheetexport` for this (`load_sheet_export_csv` in `data_io.py`
matches the 5th column by position, not name, and parses `Timestamp` as Google
Sheets' export format `M/D/YYYY H:MM:SS`, not ISO8601). The core Q10 math still works
fine on just timestamp + temperature; the only loss is the MQ-135 gas-increase
cross-check, which has nothing to compare against in this schema (always `None`).
Export the real Sheet as CSV (File > Download > CSV, or the public
`.../export?format=csv` URL the same way `read-data-from-google-sheet` reads it) and
point `--readings` at that file.

`mock_data/mock_sheet_export.csv` mirrors this real schema/format (not the richer
`mock_sheet.csv`) with synthetic data (fridge-cold rising to room temperature over 3
days) for testing `--format sheetexport --once-per-day` without touching the real
Sheet. A live demo of what the once-per-day output looks like, structured as a
separate "Predictions" tab next to a "Readings" tab (one row per reading would make a
per-reading column nonsensical at a ~30s-30min logging interval), is in the
"ENG30002 Mock Sheet - Use-By Prediction Test" Google Sheet - see `PIPELINE_PLAN.md`
for the link. Wiring an actual write-back to the live Sheet (Apps Script trigger vs.
Python pushing via the Sheets API) is still undesigned - see open risk #7.

## Scheduling

Run once or twice a day:

- **Linux/macOS (cron):** `0 8,20 * * * cd /path/to/repo && python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1`
- **Windows (Task Scheduler):** create a daily trigger (or two) that runs
  `python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1` with
  "Start in" set to the repo root.

## Running the tests

    python -m pytest prediction/tests -v

## Limitations

- **Q10 is borrowed, not measured.** It comes from Mumbre et al.'s curd data, not a
  chicken trial. See `SOURCES.md` "Q10 derivation" for the unresolved 1.69-vs-1.75
  discrepancy this implies.
- **No microbial lag phase.** The model likely over-predicts spoilage early in storage,
  since real spoilage has a slow-start phase Q10 doesn't capture.
- **Q10 assumes a constant rate-temperature relationship**, which breaks down outside
  roughly 4-25°C (`config.Q10_VALID_TEMP_RANGE_C`). Readings outside that range are
  still used in the accumulation (not dropped), but are counted
  (`q10_range_exceeded_count`) and the CLI prints a warning when any occur.
- **Expect `q10_range_exceeded_count` to fire on every normal fridge run.** A domestic
  fridge runs at roughly 1-4°C, and `config.Q10_VALID_TEMP_RANGE_C`'s lower bound is
  4.0°C, so a correctly-refrigerated trial will trip this counter on most of its
  readings. That is informational, not a fault: it flags that those intervals were
  computed by extrapolating Q10 *below* the range the source data (Mumbre et al.'s
  4-25°C) actually covers, which is a reason to trust the exact number slightly less —
  not a sign anything went wrong with the run or the sensor. A count of zero on a
  refrigerated trial would be the surprising result. Only an unexpectedly *large* count
  on a trial you believed was refrigerated (or any count on a trial held above 25°C) is
  worth investigating.
- **T_forecast is a guess** based on the mean of the last `config.FORECAST_WINDOW_HOURS`
  of readings, not an actual forecast.
- **MQ-135 vs. ammonia-specific MQ137.** Mumbre's 10/30ppm ammonia thresholds don't
  transfer to our broader-spectrum MQ-135; `config.MQ135_DANGER_RATIO` is a reused
  firmware value, not a validated one. See `SOURCES.md` "Open questions".

## Assumptions

- The Apps Script that writes the Sheet is still unverified (`PIPELINE_PLAN.md`, open
  risk #1) - we can now see the Sheet's output, not its source. `mock_sheet.csv`
  (richer schema) and `mock_sheet_export.csv` (real schema) are both still mocks, not
  a connection to the live Sheet.
- A food-start timestamp is required to accumulate equivalent hours correctly; without
  one, `resolve_food_start` falls back to the trial's earliest reading, which may
  include readings from before the chicken was actually placed in the enclosure.
