# Sources and parameter derivations

IEEE-style citation log for every literature-derived parameter in `config.py`. See
`README.md` "Limitations" for what each of these costs the model.

## References

1. T. Mumbre, T. Salunkhe, and R. Kadu, "IoT-Based Food Spoilage Detection System using
   Ammonia Sensing and Environmental Monitoring," *International Journal of
   Engineering Research & Technology (IJERT)*, vol. 15, no. 04, Apr. 2026, paper ID
   IJERTV15IS042618, doi: 10.5281/zenodo.19821237. [Online]. Available:
   https://www.ijert.org/iot-based-food-spoilage-detection-system-using-ammonia-sensing-and-environmental-monitoring-ijertv15is042618
2. P. S. Taoukis, T. P. Labuza, and I. S. Saguy, *Kinetics of Food Deterioration and
   Shelf-Life Prediction*. Boca Raton, FL, USA: CRC Press, 1997.
3. U.S. Food Safety and Inspection Service, "Cold Food Storage Chart," FoodSafety.gov.
   [Online]. Available: **TODO (Marcus): insert the verified primary FoodSafety.gov URL
   here before submission - do not cite a secondary article.**

## Parameter table

| Parameter | Value | Source | Justification |
|---|---|---|---|
| `T_REF_C` | 4.0 | [1] (refrigerated condition); [3] (chicken guidance, <=4C/40F) | Matches both the Q10 dataset and the shelf-life guidance temperature |
| `L_REF_HOURS` | 48.0 | [3], raw chicken: 1-2 days refrigerated | Chicken-specific, official food-safety authority. 48h is the upper end of the range; 24h is the conservative alternative |
| `Q10` | 1.75 | Derived from [1] via the formula in [2] | See "Q10 derivation" below - this value does not match what [1]'s own numbers compute to |
| `Q10_BAND` | (1.5, 2.0) | Our choice | Gives an earliest/latest use-by because Q10 is borrowed, not measured |
| `MQ135_DANGER_RATIO` | 5.0 | Reused from `food_monitor_compact.ino`'s `GAS_DANGER_RATIO` | Not independently validated - see "Open questions" |

## Q10 derivation

[1] reports curd (not chicken) reaching 30ppm NH3 (via an MQ137 ammonia sensor) at
approximately day 12 at 4°C and day 4 at 25°C - a reported "roughly three times
slower" rate at 4°C. Using [2]'s formula:

    Q10 = (rate ratio) ^ (10 / delta_T) = 3.0 ^ (10 / 21) ~= 1.69

This is implemented as `q10_model.derive_q10_from_ratio(3.0, 21.0)` and unit-tested in
`tests/test_q10_model.py`. **`config.Q10` is set to 1.75, not this computed 1.69** - see
"Open questions" below. `pipeline.q10_discrepancy_message()` prints both numbers every
time the CLI runs so this is never silently resolved one way or the other.

## Limitations (see also README.md)

- **Food mismatch:** [1]'s Q10 comes from curd, not chicken - borrowed for temperature
  sensitivity only, not for shelf life.
- **Temperature range:** supported only between ~4°C and ~25°C ([1]'s tested range);
  `config.Q10_VALID_TEMP_RANGE_C` records this, but readings outside it are still used.
- **Different endpoints:** [1]'s endpoint is a measured ammonia threshold; `L_REF_HOURS`
  is a food-safety guideline ([3]). A use-by date is arguably better served by the
  safety-based limit, but the two are not the same kind of measurement.
- **Sensor mismatch:** [1] used an MQ137 (ammonia-specific); this project uses an
  MQ-135 (broad VOC). [1]'s 10/30ppm thresholds do not transfer to MQ-135 readings.
- **No microbial lag phase**, and **T_forecast is a guess** from recent history, not an
  actual weather/environment forecast.

## Replacement plan

Stage 1 reference trials (chicken held at 4°C and 20°C) can give an empirical
`L_REF_HOURS` and `Q10 = (t_cold / t_warm) ^ (10 / dT)` once that data exists, computed
the same way `q10_model.derive_q10_from_ratio` already does. No task in this plan
implements the trial-data ingestion itself - see "Open questions".

## Open questions

1. **Q10: 1.75 vs. ≈1.69.** `config.Q10 = 1.75` is the literature-asserted value, but
   deriving it from [1]'s own reported numbers (3x slower, 21°C gap) via [2]'s formula
   gives ≈1.69. A ratio of ≈3.24x would be needed to get 1.75. Decide which to adopt, or
   re-derive from a closer read of [1] Fig. 3.
2. **MQ-135 disagreement threshold.** `config.MQ135_DANGER_RATIO = 5.0` is reused from
   the firmware's existing gas-ratio bands so the two systems agree by construction, but
   it was never validated against [1]'s ammonia data (different sensor, different food).
   Decide if 5.0 is right or if an independent threshold is needed.
3. **FoodSafety.gov primary URL.** Reference [3] above needs the verified primary page
   URL - retrieve and insert it before submission; do not cite a secondary article.
4. **Conservative `L_REF_HOURS`.** [3]'s range is 1-2 days; `config.L_REF_HOURS = 48.0`
   uses the upper end. Decide if a separate, stricter config value (24h) is worth adding
   now or only once real trial data suggests it's needed.
5. **Symmetric MQ-135 disagreement.** `q10_model.mq135_disagrees` only flags "model says
   Fresh, sensor says danger" (the safety-relevant direction). Decide if the reverse
   (model says Use soon/Past use-by, sensor reads well under the danger ratio) is also
   worth flagging.
6. **Real Sheet access.** Still pending - a teammate holds the link. Once available,
   confirm the Apps Script's actual column names match the HTTPS-payload schema this
   plan assumes (`PIPELINE_PLAN.md`, open risk #1) before trusting predictions run
   against the real data.
