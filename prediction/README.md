# Q10 shelf-life prediction

Predicts a use-by date for raw chicken from logged ESP32 sensor readings, using a
literature-derived Q10 temperature-acceleration model. See `SOURCES.md` for every
parameter's source, justification, and open questions.

## Usage

    pip install pytest          # only needed to run the tests
    python -m prediction.cli --readings path/to/readings.csv --trial ChickenTrial1

By default this reads the HTTPS-payload schema (what `food_monitor_compact.ino`'s
`publishToGoogleSheets()` sends). Pass `--format datacsv` to read the on-device
`DATA.CSV` schema instead. Results are appended to `predictions.csv` (override with
`--predictions-out`).

The real Google Sheet doesn't exist for us yet (a teammate holds the link) - once it
does, export it as CSV (File > Download > CSV, or the public `.../export?format=csv`
URL the same way `read-data-from-google-sheet` reads it) and point `--readings` at that
file. No code change is needed as long as the Apps Script's columns match the HTTPS
schema above.

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
- **T_forecast is a guess** based on the mean of the last `config.FORECAST_WINDOW_HOURS`
  of readings, not an actual forecast.
- **MQ-135 vs. ammonia-specific MQ137.** Mumbre's 10/30ppm ammonia thresholds don't
  transfer to our broader-spectrum MQ-135; `config.MQ135_DANGER_RATIO` is a reused
  firmware value, not a validated one. See `SOURCES.md` "Open questions".

## Assumptions

- The real Apps Script / Google Sheet is unverified (`PIPELINE_PLAN.md`, open risk #1).
  All development and testing here uses the mock data in `mock_data/mock_sheet.csv`.
- A food-start timestamp is required to accumulate equivalent hours correctly; without
  one, `resolve_food_start` falls back to the trial's earliest reading, which may
  include readings from before the chicken was actually placed in the enclosure.
