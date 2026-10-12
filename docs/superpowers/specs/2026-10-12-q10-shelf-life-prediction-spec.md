# Q10 shelf-life prediction — spec

ESP32 food-spoilage prototype (ENG30002, Group T11). The device posts readings to a
Google Sheet every 30 minutes. This spec covers a script, run once or twice a day, that
predicts a use-by date for raw chicken from the logged temperature history, using a Q10
temperature-acceleration model. Moisture/gas readings are secondary indicators, not part
of the Q10 maths.

## 1. The model

Q10 is the factor by which spoilage rate multiplies per 10°C rise.

```
k(T) = Q10 ^ ((T - T_ref) / 10)              (rate relative to T_ref)
equivalent_hours = sum( k(T_i) * dt_i )       (time banked at T_ref)
remaining_ref_hours = L_ref - equivalent_hours
predicted_use_by = now + remaining_ref_hours / k(T_forecast)
```

`T_forecast` defaults to the mean temperature over the last 12 h (configurable window).

## 2. Parameters and their sources

All values live in one config file. Nothing is hardcoded. Each parameter carries its
value, source, and justification inline, and these are also written to `SOURCES.md`.

| Parameter | Value | Source | Justification |
|---|---|---|---|
| T_ref | 4°C | Mumbre et al. (refrigerated condition); FoodSafety.gov (chicken guidance stated at 40°F/4°C or below) | Matches both the Q10 dataset and the shelf-life guidance temperature, so the model's reference point is consistent |
| L_ref | 48 h | FoodSafety.gov Cold Food Storage Chart: raw chicken 1-2 days refrigerated | Chicken-specific and from an official food-safety authority. 48 h is the upper end of the range; 24 h is the conservative alternative |
| Q10 | 1.75 (see note) | Derived from Mumbre et al., using the formula in Taoukis et al. | Our own bio-lab trials weren't possible, so temperature sensitivity is borrowed from the literature |
| Q10 band | 1.5 and 2.0 | Sensitivity range, our choice | Gives an earliest/latest use-by because Q10 is borrowed, not measured |

### Source log

1. Mumbre, T., Salunkhe, T., Kadu, R., "IoT-Based Food Spoilage Detection System using
   Ammonia Sensing and Environmental Monitoring," IJERT, vol. 15, no. 04, Apr. 2026,
   paper ID IJERTV15IS042618, DOI 10.5281/zenodo.19821237.
   https://www.ijert.org/iot-based-food-spoilage-detection-system-using-ammonia-sensing-and-environmental-monitoring-ijertv15is042618
   - Used for: the temperature-dependence of spoilage rate (Q10 derivation) and the
     reference temperature.
   - What it reports: curd (not chicken) stored at 25°C and 4°C, with spoilage defined
     as NH3 reaching 30 ppm using an MQ137 sensor. The 30 ppm boundary was reached at
     about day 4 at 25°C and about day 12 at 4°C, which the paper describes as roughly
     three times slower. Temperature gap is 21°C.
   - Retrieved by reading the paper's text. The day figures are approximate, and Fig. 3
     should be checked directly.
2. Taoukis, P. S., Labuza, T. P., Saguy, I. S., *Kinetics of Food Deterioration and
   Shelf-Life Prediction*, CRC Press, 1997. Used for: the Q10 formula,
   `Q10 = (rate ratio)^(10 / temperature difference)`.
3. U.S. FoodSafety.gov, "Cold Food Storage Chart" (raw chicken, whole or parts: 1-2 days
   in the refrigerator). Used for: L_ref. **The user must retrieve and cite the primary
   page — do not cite secondary articles.**

### Justification and limitations (must appear in README and code comments)

- **Q10 value:** a 3x rate difference over 21°C gives `3^(10/21) ≈ 1.69`, not 1.75.
  1.75 would need a ratio of about 3.2x. Compute the value from stored inputs (ratio,
  temperature gap) in the config so the derivation is traceable, show both numbers, and
  ask which to adopt. Do not silently pick one.
- **Food mismatch:** the Q10 comes from curd, not chicken. Treat it as a borrowed
  assumption for temperature sensitivity only, not for shelf life.
- **Temperature range:** the Q10 is supported only between about 4°C and 25°C. Warn when
  readings fall outside that range.
- **Different endpoints:** the Mumbre endpoint is a measured ammonia threshold, while
  L_ref is a food-safety guideline. For a use-by date the safety-based limit is arguably
  appropriate, but state the difference.
- **Sensor mismatch:** Mumbre used an MQ137 (ammonia). We use an MQ-135, so Mumbre's ppm
  thresholds (10/30 ppm) do not transfer. Our MQ-135 threshold is still open and is a
  config placeholder.
- **Model limits:** no microbial lag phase, so early spoilage is likely over-predicted.
  Forecast temperature is a guess from recent history.
- **Replacement plan:** Stage 1 reference trials (chicken at 4°C and 20°C) can give an
  empirical L_ref and `Q10 = (t_cold / t_warm)^(10 / dT)`. Config should let these be
  swapped in later, with a documented threshold for when to do so.

### Traceability requirement

Every prediction row written to output must record the parameter set used (T_ref,
L_ref, Q10, forecast window, config version), so any prediction can be traced back to
its sources and re-run if the parameters change.

## 3. Input data

Neela holds the real Sheet link; it doesn't exist for us yet. Three schemas exist in the
repo:

- The HTTPS payload schema `food_monitor_compact.ino`'s `publishToGoogleSheets()` sends
  today (`mq135, temp, humidity, pressure, lux, gas_base, gas_increase, risk, trial,
  source, device_ts`), plus a server-side receipt timestamp — **this is the schema the
  mock data uses**, since it's the best proxy for what a correctly-written Apps Script
  would actually need to store.
- The on-device SD card `DATA.CSV` schema (`date, time, trial, temp_C, humidity_pct,
  pressure_hPa, lux, mq135_raw, mq135_base, mq135_increase, source`) — supported as a
  backup loader via column mapping.
- An older `TESTLOG.CSV` schema sitting on `origin/dev` — not used; superseded by the
  above two.

Parse by header name, not column position. `PIPELINE_PLAN.md` notes the real Apps
Script is still unverified — this is an explicit assumption of this plan, not something
it resolves.

Food start time: a config value, falling back to the first timestamp of the current
trial's readings.

## 4. Behaviour

1. Load readings after the start time, sorted.
2. Compute dt from real timestamp gaps. Cap dt (default 2 h) so outages don't
   over-count, and report gaps.
3. Flag/drop implausible temperatures (missing, sensor error, out of sensor range).
4. Output: equivalent hours, % of shelf life used, remaining hours, use-by (central plus
   Q10 band), and status (Fresh / Use soon / Past use-by, thresholds in config).
5. Show the MQ-135 reading alongside as an independent indicator, and flag when it
   disagrees with the model (model says Fresh but MQ-135 has crossed its threshold —
   threshold TBD).
6. Append results to a predictions CSV.

## 5. Structure and constraints

- Pure functions for the Q10 maths, separate from sheet I/O, with unit tests (`k = 1` at
  `T_ref`, `k = Q10` at `T_ref + 10`, hand-checkable accumulation).
- CLI entry point suitable for scheduling once or twice a day.
- Simple, readable code — student project, must be able to explain every line. Comments
  factual and referenced, not filler.
- All references in IEEE style; all sources real and verifiable.

## 6. Open questions going into planning

1. Q10 = 1.75 (literature-asserted) vs. ≈1.69 (computed from Mumbre's own reported
   numbers). Do not silently pick one.
2. MQ-135 disagreement threshold — TBD, config placeholder.
