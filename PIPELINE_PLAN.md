# Data & alerting pipeline — status and plan

Living doc for the team. Target end-to-end pipeline:

```
ESP32 device  --Wi-Fi-->  Google Sheet  --Q10 model-->  predicted use-by date
                                         --daily-->       Telegram message
```

Last updated: 6 Oct 2026. Branch: `dev`.

## What exists today

| Piece | File | State |
|---|---|---|
| Main firmware (sensors, SD log, display, camera, MQTT) | `food_monitor_compact.ino` | Working, now also sends to Google Sheets (this change) |
| Minimal HTTPS→Apps Script prototype | `google-sheet.ino` | Working proof-of-concept a teammate wrote; not run on the real device, kept in-repo as reference |
| Legacy read-only sheet poller | `read-data-from-google-sheet` | Superseded by the new schema below; candidate to repurpose as an offline MAE-validation script later |
| Apps Script (writes the Sheet) | *not in this repo* | Deployed in a teammate's Google account. Confirmed only by them saying they've seen the Sheet update live — **nobody on our side has read its code yet.** Treat every assumption about its column names/logic as unverified until that happens. |
| Prediction model | — | Doesn't exist yet. Next major piece of work. |
| Daily alert | — | Doesn't exist yet. |

## Decisions made this session

- **Transport**: keep MQTT exactly as-is (local Home Assistant dashboard, zero risk to touch). Add a second, independent path: the device now also POSTs sensor readings to the Apps Script URL over HTTPS, piggybacking on the same log/manual/startup measurement events that already write to `DATA.CSV` and MQTT.
- **Compatibility over elegance**: the new HTTPS call reuses the exact GET parameter names the teammate's script is already assumed to read (`mq135`, `temp`, `humidity`, `pressure`), so it won't break whatever's currently working. New fields (`lux`, `gas_base`, `gas_increase`, `risk`, `trial`, `source`, `device_ts`) are appended additively — an unmodified script just ignores them until it's extended to use them.
- **Sheet layout**: one shared tab with a `trial` column (matches the existing `DATA.CSV` convention), not one tab per trial.
- **Timestamps**: device RTC now self-corrects on boot (if the battery died or the clock is behind the firmware's build time, it resets to build time) — fixes the old "year 1980" bug. Once the Apps Script is actually visible, plan is to have *it* stamp server-side receipt time as the authoritative timestamp (Australia/Sydney), with the device's RTC time kept as a secondary field.
- **Model**: a real Q10-based (≈1.75, Taoukis formula) spoilage model, extrapolating the gas-ratio trajectory + temperature to predict when the ratio crosses the DANGER (≥5x) threshold. Will live in Apps Script (daily time-driven trigger), once we've confirmed we can actually edit it.
- **Validation**: ground truth is currently informal — chicken assumed spoiled by smell/24h-out-of-fridge by end of trial runs, with biolab-grade testing pending approval. MAE will be computed against this informal label and reported honestly as a limitation, not dressed up as rigorous.
- **Alert channel**: Telegram (bot via @BotFather), once daily, sent from Apps Script via `UrlFetchApp` once that script is extended with the model.
- **Secrets**: Telegram bot token + chat ID go in Apps Script Script Properties, never in source. Wi-Fi credentials live only in `/config.txt` on the SD card, never hardcoded in the `.ino` (a real Wi-Fi password was found hardcoded in a newer firmware draft and stripped before this commit).
- **Apps Script in version control**: plan to pull it into this repo via `clasp` once we have edit access, so the model/trigger/alert logic is actually reviewable and citable in the report.

## What changed in this commit

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

1. **Apps Script is unverified.** Nobody but the teammate has seen its code. This blocks the model/trigger/Telegram work, not the firmware work (which is why firmware went first). **Needs resolving by Wed 14 Oct** — get edit access or a source dump from the teammate. If that slips, fallback is standing up our own Apps Script from `google-sheet.ino`'s pattern.
2. Ground-truth spoilage labels are informal; biolab approval is pending and may not land before 26 Oct. MAE may end up qualitative/limitation-only.
3. MQ135 temperature/humidity cross-sensitivity still confounds the 4°C vs 20°C comparison (inherent sensor limitation, not fixable in software).
4. The HTTPS call uses `client.setInsecure()` (skips TLS certificate validation) — accepted practical risk on a trusted LAN, called out as a limitation in the report rather than fixed.
5. The Apps Script `exec` URL is now in git history (not a credential, but visible to anyone with repo access).
6. The RTC fix only helps once a board is actually reflashed with this firmware — verify in the field before relying on it for a real trial.

## Task list to 26 Oct (Sydney time)

**Week 1 (6–12 Oct)**
- [x] 6 Oct — Firmware HTTPS path added, committed to `dev`.
- [ ] 7–8 Oct — Get teammate to share the Apps Script source/access; set up Telegram bot + chat ID.
- [ ] 8–10 Oct — Bench-test the new firmware payload against the live endpoint.
- [ ] 10–12 Oct — Extend Apps Script to parse/store the new fields; switch to server-side Sydney timestamp.

**Week 2 (13–19 Oct)**
- [ ] 13–15 Oct — Implement Q10 prediction logic in Apps Script.
- [ ] 15–16 Oct — Wire Telegram push + daily trigger; secrets into Script Properties.
- [ ] 16–18 Oct — End-to-end bench test (device → sheet → model → Telegram) over a multi-hour run.
- [ ] 18–19 Oct — `clasp`-pull Apps Script into this repo; merge `dev` → `main` once verified.

**Week 3 (20–26 Oct)**
- [ ] 20–22 Oct — Finish real trial replicates if the biolab slot lands; otherwise finalize on existing demo-run data; compute/document MAE.
- [ ] 22–23 Oct — Record the 5-min demo video.
- [ ] 23–25 Oct — Write up the ~10,000-word report.
- [ ] 25–26 Oct — Buffer, rehearsal, submit/present.
