# Data & alerting pipeline — status and plan

Living doc for the team. Target end-to-end pipeline:

```
ESP32 device  --Wi-Fi-->  Google Sheet  --Q10 model-->  predicted use-by date
                                         --daily-->       Telegram message
```

Last updated: 12 Oct 2026. Branch: `dev`.

## Team split (as of 12 Oct)

- **Marcus**: owns the Q10 prediction model (reads logged sensor data, predicts a
  use-by date). This is the piece documented below as "Prediction model".
- **Teammates** (Sanjida, Afrin, Neela, Luth, Muhammadh, Shafiu, Del, Liu — group T11):
  own the device↔Sheet communication, drawing on their prior-assignment MQTT/Wi-Fi
  experience. They have not yet confirmed whether writing to the Sheet is actually
  working end-to-end.
- **Neela** holds the real Google Sheet's link — not yet shared with Marcus. The
  prediction model is being built and tested against mock data in the meantime (see
  below); nobody has swapped in the real Sheet yet.

## What exists today

| Piece | File | State |
|---|---|---|
| Main firmware (sensors, SD log, display, camera, MQTT) | `food_monitor_compact.ino` | Working. Currently tracks a teammate's bench-test build (fast gas-calibration timing) merged on top of the Google Sheets HTTPS feature — see "Decisions made this session" below. |
| Minimal HTTPS→Apps Script prototype | `google-sheet.ino` | Working proof-of-concept a teammate wrote; not run on the real device, kept in-repo as reference |
| Legacy read-only sheet poller | `read-data-from-google-sheet` | Superseded by the new schema below; candidate to repurpose as an offline MAE-validation script later |
| Apps Script (writes the Sheet) | *not in this repo* | Deployed in a teammate's Google account. **Still unverified** — nobody on our side has read its code. Treat every assumption about its column names/logic as unverified until that happens. |
| Prediction model | `prediction/` (Python package) | **Done and merged to `dev`** (12 Oct). Q10 temperature-acceleration model, CLI entry point, 29 passing tests. Tested against a synthetic mock CSV, not the real Sheet (which doesn't exist for us yet — see team split above). Architecture note: this ended up as a standalone Python script/CLI rather than living inside Apps Script as originally sketched on 6 Oct — see below. |
| Daily alert | — | Doesn't exist yet. |

## Decisions made 12 Oct

- **Prediction model lives in its own Python package (`prediction/`), not Apps
  Script.** Supersedes the 6 Oct note below ("Will live in Apps Script"). Reasons: the
  real Sheet/Apps Script still isn't accessible (see team split above), and a
  stdlib-only Python CLI is easier to TDD, unit-test, and explain line-by-line for the
  report than Apps Script JS. It reads a CSV (either a real Sheet export or the SD
  card's `DATA.CSV` as backup) and is meant to run once/twice a day via cron/Task
  Scheduler, not as an Apps Script trigger. **Open question:** how this now connects to
  the Telegram alert step below — original plan had Apps Script push Telegram
  directly; that integration point hasn't been redesigned yet for the standalone-script
  approach.
- **Q10 value is still unresolved.** The literature-asserted 1.75 doesn't match what
  the same literature's own numbers compute to (≈1.69 via Taoukis's formula from
  Mumbre et al.'s reported ratio). Currently shipped as 1.75 with a loud runtime
  warning on every run, specifically so it's never silently picked one way or the
  other. See `prediction/SOURCES.md` "Open questions" for the full writeup and five
  other open items (MQ-135 threshold, FoodSafety.gov citation, etc.).
- **`food_monitor_compact.ino` on `main` now reflects a teammate's actual bench-test
  device build** (sent via Teams, not git — they don't use GitHub), merged in after
  confirming it was older than `dev`'s copy and had zero genuine new logic beyond faster
  gas-calibration timing for easier bench testing. `dev`'s copy now uses that same fast
  timing. The teammate's real Wi-Fi credentials are in `main`'s git history (team's
  explicit choice, not a mistake) — not in `dev`'s.

## Decisions made 6 Oct

- **Transport**: keep MQTT exactly as-is (local Home Assistant dashboard, zero risk to touch). Add a second, independent path: the device now also POSTs sensor readings to the Apps Script URL over HTTPS, piggybacking on the same log/manual/startup measurement events that already write to `DATA.CSV` and MQTT.
- **Compatibility over elegance**: the new HTTPS call reuses the exact GET parameter names the teammate's script is already assumed to read (`mq135`, `temp`, `humidity`, `pressure`), so it won't break whatever's currently working. New fields (`lux`, `gas_base`, `gas_increase`, `risk`, `trial`, `source`, `device_ts`) are appended additively — an unmodified script just ignores them until it's extended to use them.
- **Sheet layout**: one shared tab with a `trial` column (matches the existing `DATA.CSV` convention), not one tab per trial.
- **Timestamps**: device RTC now self-corrects on boot (if the battery died or the clock is behind the firmware's build time, it resets to build time) — fixes the old "year 1980" bug. Once the Apps Script is actually visible, plan is to have *it* stamp server-side receipt time as the authoritative timestamp (Australia/Sydney), with the device's RTC time kept as a secondary field.
- **Model**: a real Q10-based (≈1.75, Taoukis formula) spoilage model, extrapolating the gas-ratio trajectory + temperature to predict when the ratio crosses the DANGER (≥5x) threshold. Will live in Apps Script (daily time-driven trigger), once we've confirmed we can actually edit it.
- **Validation**: ground truth is currently informal — chicken assumed spoiled by smell/24h-out-of-fridge by end of trial runs, with biolab-grade testing pending approval. MAE will be computed against this informal label and reported honestly as a limitation, not dressed up as rigorous.
- **Alert channel**: Telegram (bot via @BotFather), once daily, sent from Apps Script via `UrlFetchApp` once that script is extended with the model.
- **Secrets**: Telegram bot token + chat ID go in Apps Script Script Properties, never in source. Wi-Fi credentials live only in `/config.txt` on the SD card, never hardcoded in the `.ino` (a real Wi-Fi password was found hardcoded in a newer firmware draft and stripped before this commit).
- **Apps Script in version control**: plan to pull it into this repo via `clasp` once we have edit access, so the model/trigger/alert logic is actually reviewable and citable in the report.

## What changed 12 Oct

- **`prediction/` package added and merged to `dev`.** Q10 temperature-acceleration
  model (`q10_model.py`, pure functions, unit-tested), config with every parameter
  sourced (`config.py`), two CSV input schemas + a predictions-CSV writer
  (`data_io.py`), orchestration (`pipeline.py`), a CLI entry point (`cli.py`), a
  16-row synthetic mock dataset exercising gaps/dropped-readings/band edge cases, 29
  passing tests, and `prediction/README.md` / `prediction/SOURCES.md`. Usage and full
  citation/limitations list live in those two files — not duplicated here.
- **Caught and fixed before merge**: the Q10 sensitivity band (earliest/latest use-by)
  was inverted below the 4°C reference temperature — i.e. in normal fridge storage,
  this device's primary real condition. Fixed and covered by a regression test; see
  `prediction/pipeline.py`'s `run_prediction()`.
- **`main`'s `food_monitor_compact.ino` updated** to a teammate's current bench-test
  build (see "Decisions made 12 Oct" above); `dev`'s copy had its gas-calibration
  timing matched to it.
- **Next step for the prediction model**: point `prediction/cli.py --readings` at a
  real Sheet CSV export once Neela shares the link, instead of
  `prediction/mock_data/mock_sheet.csv`. No code change needed if the real Sheet's
  columns match the HTTPS-payload schema the firmware already sends — confirm that
  once the Apps Script is visible (open risk #1 below).

## What changed 6 Oct

`food_monitor_compact.ino`:
- New optional `config.txt` field: `GOOGLE_SCRIPT_URL` (blank = feature disabled). `CONFIG_VERSION` bumped 1→2, so an **old `config.txt` will stop loading** and the device will fall back to its existing default-config/serial-setup screen — this is expected, not a bug. Re-run serial setup (or hand-edit `config.txt`) to add the new field.
- New `publishToGoogleSheets()` function, called everywhere MQTT telemetry is already published (scheduled logs, manual button sample, startup sample).
- `google-sheet.ino` added to the repo as the reference prototype this is based on.

## How to run / configure it

1. Flash `food_monitor_compact.ino` as before (Arduino IDE/CLI, ESP32 board).
2. On first boot with the new firmware, an old `config.txt` will be rejected (version mismatch) — use the on-device serial setup wizard (connect at 115200 baud, follow prompts) to regenerate it, or hand-edit `/config.txt` on the SD card to add a line:
   ```
   CONFIG_VERSION=2
   GOOGLE_SCRIPT_URL=https://script.google.com/macros/s/.../exec
   ```
   (copy the other existing fields unchanged; leave `GOOGLE_SCRIPT_URL=` blank to disable Sheets posting entirely).
3. MQTT/Home Assistant discovery, SD logging, the display, and the camera are all unchanged — nothing to reconfigure there.

## Open risks

1. **Apps Script is unverified.** Nobody but the teammate has seen its code. This blocks the Telegram/trigger work, and now also blocks pointing the prediction model at real data instead of mock data. **Needs resolving ASAP** — get edit access or a source dump from the teammate. If that slips, fallback is standing up our own Apps Script from `google-sheet.ino`'s pattern.
2. Ground-truth spoilage labels are informal; biolab approval is pending and may not land before 26 Oct. MAE may end up qualitative/limitation-only.
3. MQ135 temperature/humidity cross-sensitivity still confounds the 4°C vs 20°C comparison (inherent sensor limitation, not fixable in software).
4. The HTTPS call uses `client.setInsecure()` (skips TLS certificate validation) — accepted practical risk on a trusted LAN, called out as a limitation in the report rather than fixed.
5. The Apps Script `exec` URL is now in git history (not a credential, but visible to anyone with repo access).
6. The RTC fix only helps once a board is actually reflashed with this firmware — verify in the field before relying on it for a real trial.
7. **How the standalone `prediction/` script reaches Telegram is undesigned.** The original plan had Apps Script push Telegram directly once it had the model built in; now that the model is a separate Python CLI, someone needs to decide whether Apps Script still owns the Telegram push (reading the script's predictions some other way) or the Python CLI does it directly via `UrlFetchApp`'s Telegram-bot-API equivalent in Python.
8. `prediction/SOURCES.md` lists 6 open questions (Q10 1.75 vs ≈1.69, MQ-135 threshold, the FoodSafety.gov citation URL still TODO, etc.) — worth a look before the model's numbers go in the report.

## Task list to 26 Oct (Sydney time)

**Week 1 (6–12 Oct)**
- [x] 6 Oct — Firmware HTTPS path added, committed to `dev`.
- [x] 12 Oct — Prediction model (`prediction/` Python package) built, tested against mock data, merged to `dev`. Done ahead of the original Week 2 slot, against mock data since the real Sheet still isn't accessible.
- [ ] Get teammate (Neela) to share the real Sheet link; confirm with whoever owns the Apps Script that its columns match what the firmware sends.
- [ ] Bench-test the firmware's HTTPS payload against the live endpoint, once reachable.
- [ ] Decide the Telegram integration shape now that the model is a standalone script, not Apps Script (open risk #7).

**Week 2 (13–19 Oct)**
- [x] ~~13–15 Oct — Implement Q10 prediction logic in Apps Script.~~ Superseded — done
  12 Oct as a standalone Python package instead (see "What changed 12 Oct" above).
- [ ] 13–15 Oct — Point `prediction/cli.py` at the real Sheet CSV export once Neela
  shares the link; confirm its columns against the HTTPS-payload schema.
- [ ] 15–16 Oct — Wire Telegram push + scheduling (cron/Task Scheduler for the Python
  CLI, or still via Apps Script — decide per open risk #7); secrets into whichever
  side owns the Telegram call.
- [ ] 16–18 Oct — End-to-end bench test (device → sheet → model → Telegram) over a multi-hour run.
- [ ] 18–19 Oct — Merge `dev` → `main` once the real-Sheet path is verified.

**Week 3 (20–26 Oct)**
- [ ] 20–22 Oct — Finish real trial replicates if the biolab slot lands; otherwise finalize on existing demo-run data; compute/document MAE.
- [ ] 22–23 Oct — Record the 5-min demo video.
- [ ] 23–25 Oct — Write up the ~10,000-word report.
- [ ] 25–26 Oct — Buffer, rehearsal, submit/present.
