# ENG30002 — Dynamic Use-By Date

Swinburne ENG30002 (2026), Group T11. An ESP32-based food-spoilage sensing prototype
that logs fridge/room conditions, predicts a use-by date from that data using a
temperature-acceleration model, and (eventually) sends a daily alert.

```
ESP32 device  --Wi-Fi-->  Google Sheet  --Q10 model-->  predicted use-by date
                                         --daily-->       Telegram message
```

`PIPELINE_PLAN.md` is the living, actively-updated status doc for this project — read
it for exactly what's built, what's not, and what's open. This README is a map of how
the pieces fit together and where to find each one; it isn't updated every session the
way `PIPELINE_PLAN.md` is.

## How it works

### 1. The device

`food_monitor_compact.ino` runs on an ESP32 with a BME280 (temperature, humidity,
pressure), an MQ135 gas sensor, a BH1750 light sensor, an SD card, an ST7735 display,
and a camera. It logs a reading roughly every 30 minutes on a schedule (configurable),
plus on a manual button press or at startup, and writes each reading three places at
once: the SD card (`DATA.CSV`), the team's MQTT/Home Assistant dashboard, and — over
HTTPS — a Google Apps Script endpoint that writes it into a Google Sheet.
`google-sheet.ino` is a minimal reference prototype for that last path.

### 2. The Google Sheet

The Apps Script that receives the device's HTTPS posts and writes them into the Sheet
lives in a teammate's Google account, outside this repo, and **its source has not been
read by anyone on the prediction side** — treat its exact behaviour as unverified. What
*is* confirmed (by viewing the Sheet directly, 13 Oct) is its actual output schema:
`Timestamp, MQ135, Temp, Humidity`, plus a 5th column with no header text (pressure) —
narrower than the firmware's full HTTPS payload. See `prediction/README.md` → "The real
Sheet" for exactly how the prediction code accounts for that.

### 3. The prediction model

`prediction/` is a standalone, stdlib-only Python package (deliberately *not* Apps
Script JS — see `PIPELINE_PLAN.md`'s "Decisions made 12 Oct" for why) that reads logged
readings and predicts a use-by date using a Q10 temperature-acceleration model: spoilage
rate is modelled as multiplying by a factor `Q10` for every 10°C above a reference
temperature (4°C, matching fridge storage), so time spent warmer than the fridge "costs"
disproportionately more of the food's shelf life than the same time spent cold. Every
reading's temperature is converted into "equivalent hours at 4°C" and summed; once that
sum reaches the reference shelf life (48h, from FoodSafety.gov's raw-chicken guidance),
the model calls it past due. Because the Q10 value itself is borrowed from an unrelated
published dataset rather than measured for chicken, every prediction is reported as a
range (not a single date) and the model prints a loud warning about the literature's own
internal inconsistency on every run — see `prediction/SOURCES.md` for the full citation
trail and every open question.

Run it directly:

```
python -m prediction.cli --readings path/to/readings.csv --trial ChickenTrial1
```

Full usage, the three supported input schemas, scheduling, and limitations are in
`prediction/README.md`.

### 4. Getting predictions into the Sheet, automatically

As of 13 Oct, `prediction/sheets_daily_run.py` + `.github/workflows/daily-prediction.yml`
implement an unattended version of this: a GitHub Actions job, scheduled for 8am and 8pm
Sydney time, reads the Sheet's Readings tab via the Google Sheets API, runs the same
prediction model, and appends one row per day to a Predictions tab — gated so a prediction
already recorded for today isn't duplicated if the job fires twice. **This is implemented
and tested but not yet live**: it needs a one-time Google Cloud service-account setup, the
Sheet owner granting that account Editor access, the food-start time configured, and the
workflow file to be on the repository's default branch before the schedule actually writes
anything. Full setup steps and a manual smoke-test procedure are in `prediction/README.md`
→ "Unattended daily write-back (GitHub Actions)"; the design rationale (why GitHub Actions
+ the Sheets API rather than an Apps Script trigger) is in
`docs/superpowers/specs/2026-10-13-sheets-write-back-design.md`.

A standalone demo of what this looks like — a "Readings" tab and a "Predictions" tab
showing three days of output as mock sensor data warms from fridge to room temperature
— is linked from `PIPELINE_PLAN.md`'s "What changed 13 Oct" section.

### 5. The daily alert

Not built yet. The plan is a Telegram message once a day once a prediction exists, but
exactly which system sends it (the GitHub Actions job above vs. the Apps Script) is an
open decision — see `PIPELINE_PLAN.md`, open risk #7.

## Repo layout

| Path | What it is |
|---|---|
| `food_monitor_compact.ino` | Main firmware: sensors, SD logging, display, camera, MQTT, HTTPS-to-Sheet |
| `google-sheet.ino` | Minimal reference prototype for the HTTPS-to-Apps-Script path |
| `read-data-from-google-sheet` | Legacy read-only Sheet poller, superseded by `prediction/`'s loaders |
| `prediction/` | The Q10 prediction model: pure math (`q10_model.py`), config (`config.py`), CSV/Sheets I/O (`data_io.py`, `sheets_sync.py`), orchestration (`pipeline.py`, `cli.py`, `sheets_daily_run.py`), tests, and its own `README.md`/`SOURCES.md` |
| `.github/workflows/daily-prediction.yml` | The scheduled GitHub Actions job described above |
| `PIPELINE_PLAN.md` | **The authoritative, dated, living status/plan doc — read this first for current state** |
| `docs/superpowers/specs/`, `docs/superpowers/plans/` | Design specs and implementation plans written before building non-trivial pieces (e.g. the Sheet write-back) |

## Running the tests

```
pip install -r requirements.txt   # only needed for the Sheets-API modules
python -m pytest prediction/tests -v
```

## Team

Group T11. Marcus owns the Q10 prediction model (this repo's `prediction/` package); the
rest of the team owns the device↔Sheet communication. See `PIPELINE_PLAN.md` for the
current team split, task list, and report/demo deadline (26 Oct 2026, Sydney time).
