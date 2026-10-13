# ENG30002 — Dynamic Use-By Date

Swinburne ENG30002 (2026), Group T11. An ESP32-based food-spoilage sensing prototype
that logs fridge/room conditions, predicts a use-by date from that data using a
temperature-acceleration model, and (eventually) sends a daily alert.

```
ESP32 device  --Wi-Fi-->  Google Sheet  --Q10 model-->  predicted use-by date
                                         --daily-->       Telegram message
```

**Start here:** `PIPELINE_PLAN.md` is the living, actively-updated status doc — it has
exactly what's built, what's not, and what's open. This README doesn't change every
session; it's a map of how the four pieces below fit together and where to find each
one in the repo.

## Status at a glance

| Piece | State |
|---|---|
| 1. Device (firmware) | Working — logs readings and posts them to the Sheet |
| 2. Google Sheet | Receiving real data; the Apps Script that writes it is unverified (nobody's read its source) |
| 3. Prediction model | Done and merged — tested against mock data matching the real Sheet's schema |
| 4. Sheet write-back | Built and tested, **not yet switched on** — needs one-time setup (below) |
| 5. Daily alert | Not built yet |

Details on each below; `PIPELINE_PLAN.md` has the full dated history.

## How it works

### 1. The device

`food_monitor_compact.ino` runs on an ESP32 with:

- a BME280 (temperature, humidity, pressure)
- an MQ135 gas sensor
- a BH1750 light sensor
- an SD card, an ST7735 display, and a camera

It logs a reading roughly every 30 minutes (configurable), plus on a manual button
press or at startup. Each reading is written to three places at once:

1. the SD card (`DATA.CSV`)
2. the team's MQTT/Home Assistant dashboard
3. a Google Apps Script endpoint, over HTTPS, which writes it into a Google Sheet

`google-sheet.ino` is a minimal reference prototype for that third path, if you just
want to see the HTTPS-to-Sheet piece in isolation.

### 2. The Google Sheet

The Apps Script that receives the device's HTTPS posts and writes them into the Sheet
lives in a teammate's Google account, outside this repo. **Nobody on the prediction
side has read its source** — treat its exact behaviour as unverified.

What *is* confirmed, by viewing the Sheet directly (13 Oct), is its output schema:

```
Timestamp, MQ135, Temp, Humidity, <unnamed 5th column = pressure>
```

That's narrower than the firmware's full HTTPS payload. See `prediction/README.md` →
"The real Sheet" for exactly how the prediction code accounts for the gap.

### 3. The prediction model

`prediction/` is a standalone, stdlib-only Python package — deliberately plain Python
rather than Apps Script JS (see `PIPELINE_PLAN.md`'s "Decisions made 12 Oct" for why).

**The idea in one paragraph:** spoilage speeds up with temperature. The model uses a
Q10 value to say *how much* faster — every 10°C above a 4°C fridge-cold reference
roughly multiplies the spoilage rate by Q10. Each reading's temperature gets converted
into "equivalent hours at 4°C" and summed up. Once that running total reaches the
reference shelf life (48h, from FoodSafety.gov's raw-chicken guidance), the model
calls the food past due.

**Why it's a range, not one date:** the Q10 value itself is borrowed from an unrelated
published dataset, not measured for chicken specifically. So every prediction is
reported as a range (earliest/latest), and the model prints a warning on every run
about the literature's own inconsistency. Full citation trail and open questions are
in `prediction/SOURCES.md`.

Run it directly:

```
python -m prediction.cli --readings path/to/readings.csv --trial ChickenTrial1
```

Full usage, the three supported input schemas, scheduling, and limitations are in
`prediction/README.md`.

### 4. Getting predictions into the Sheet, automatically

`prediction/sheets_daily_run.py` + `.github/workflows/daily-prediction.yml` run the
prediction model automatically, twice a day (8am and 8pm Sydney time), and write one
row per day to the Sheet's **Predictions** tab via the Google Sheets API. A
once-per-day gate stops a duplicate row if the job happens to fire twice.

**This is built and tested, but not live yet.** Before the schedule will actually
write anything, someone needs to:

1. Create a Google Cloud service account and grant it Editor access to the real Sheet
2. Set the `GOOGLE_SERVICE_ACCOUNT_KEY` and `SPREADSHEET_ID` repo secrets/variables
3. Set the food-start time in config
4. Merge the workflow file onto the repo's default branch (scheduled triggers only
   fire from there)

Full setup steps and a manual smoke-test checklist are in `prediction/README.md` →
"Unattended daily write-back (GitHub Actions)". The design rationale (why GitHub
Actions + the Sheets API rather than an Apps Script trigger) is in
`docs/superpowers/specs/2026-10-13-sheets-write-back-design.md`.

A standalone demo of what the Predictions tab looks like in practice — three days of
mock sensor data warming from fridge to room temperature — is linked from
`PIPELINE_PLAN.md`'s "What changed 13 Oct" section.

### 5. The daily alert

Not built yet. The plan is a Telegram message once a day once a prediction exists,
but which system ends up sending it (the GitHub Actions job above, or the Apps
Script) is still an open decision — see `PIPELINE_PLAN.md`, open risk #7.

## Repo layout

| Path | What it is |
|---|---|
| `food_monitor_compact.ino` | Main firmware: sensors, SD logging, display, camera, MQTT, HTTPS-to-Sheet |
| `google-sheet.ino` | Minimal reference prototype for the HTTPS-to-Apps-Script path |
| `read-data-from-google-sheet` | Legacy read-only Sheet poller, superseded by `prediction/`'s loaders |
| `telegram-bot-integration` | A teammate's prototype for the daily-alert piece (open risk #7 above); not yet wired to the prediction model's output |
| `prediction/` | The Q10 prediction model: pure math (`q10_model.py`), config (`config.py`), CSV/Sheets I/O (`data_io.py`, `sheets_sync.py`), orchestration (`pipeline.py`, `cli.py`, `sheets_daily_run.py`), tests, and its own `README.md`/`SOURCES.md` |
| `.github/workflows/daily-prediction.yml` | The scheduled GitHub Actions job described in step 4 above |
| `PIPELINE_PLAN.md` | **The authoritative, dated, living status/plan doc — read this first for current state** |
| `docs/superpowers/specs/`, `docs/superpowers/plans/` | Design specs and implementation plans written before building non-trivial pieces (e.g. the Sheet write-back) |

## Running the tests

```
pip install -r requirements.txt   # only needed for the Sheets-API modules
python -m pytest prediction/tests -v
```

## Team

Group T11. Marcus owns the Q10 prediction model (this repo's `prediction/` package);
the rest of the team owns the device-to-Sheet communication. See `PIPELINE_PLAN.md`
for the current team split, task list, and the report/demo deadline (26 Oct 2026,
Sydney time).
