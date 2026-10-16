"""All tunable parameters for the use-by prediction model.

Nothing in q10_model.py, data_io.py, or pipeline.py is hardcoded - every
number that affects a prediction lives here, with its source and
justification as a comment. See SOURCES.md for full citations and
README.md "Limitations" for what each assumption costs us.
"""
from datetime import datetime
from typing import Optional

# --- Q10 model reference point -------------------------------------------

# Source: Mumbre et al. (refrigerated condition) and US FoodSafety.gov
# chicken guidance (<=4C / 40F). Chosen so the model's reference
# temperature matches both the borrowed Q10 dataset and the food-safety
# guidance temperature.
T_REF_C = 4.0

# Source: US FoodSafety.gov Cold Food Storage Chart, raw chicken
# (whole or parts): 1-2 days refrigerated. 48h is the upper end of that
# range; 24h is the conservative alternative if you want to be stricter.
L_REF_HOURS = 48.0

# --- Q10 value and its derivation (see SOURCES.md "Q10 derivation") -----

# Mumbre et al.: curd reached 30ppm NH3 (MQ137) at ~day 12 at 4C and
# ~day 4 at 25C, a reported "roughly three times slower" at 4C.
Q10_LITERATURE_RATE_RATIO = 3.0
Q10_LITERATURE_DELTA_T_C = 21.0

# NOTE: Q10_LITERATURE_RATE_RATIO/DELTA_T_C computes to Q10 ~= 1.69, not
# 1.75 (see prediction/q10_model.py:derive_q10_from_ratio and
# SOURCES.md "Q10 derivation"). Left at 1.75 pending a decision - see
# SOURCES.md "Open questions". pipeline.py prints a warning every run
# until this is resolved; it is never silently swapped for the computed
# value.
Q10 = 1.75

# Sensitivity band: since Q10 is borrowed rather than measured, every
# prediction also reports what it would be at these two values, giving
# an earliest/latest use-by range. Our choice, not from a source.
Q10_BAND = (1.5, 2.0)

# Q10 extrapolation is only physically supported in the range the source
# data covers. Outside this, rate_factor() is a guess, not a measurement.
Q10_VALID_TEMP_RANGE_C = (4.0, 25.0)

# --- Forecasting and timestamp handling ----------------------------------

# How far back to average temperature for T_forecast.
FORECAST_WINDOW_HOURS = 12.0

# A gap longer than this (e.g. Wi-Fi outage) is capped so one long outage
# doesn't silently bank hours of "equivalent spoilage" that never happened.
MAX_DT_HOURS = 2.0

# Sensor sanity range for temp_c - readings outside this are a sensor
# fault, not real data, and are dropped. This is NOT a food-safety bound.
PLAUSIBLE_TEMP_RANGE_C = (-10.0, 60.0)

# Set to a specific `datetime` to force a food-start time; leave as None
# to fall back to the earliest reading's timestamp for the trial (see
# pipeline.resolve_food_start). Can also be overridden per run with the
# CLI's --food-start flag.
FOOD_START_TS: Optional[datetime] = None

# --- Secondary indicator: MQ-135 gas ratio --------------------------------

# OPEN QUESTION (see SOURCES.md): reused from food_monitor_compact.ino's
# GAS_DANGER_RATIO so the two systems agree by construction, but the
# MQ-135 was never validated against a real spoilage endpoint for this
# project - Mumbre's 10/30ppm thresholds are for a different sensor
# (MQ137, ammonia-specific) and don't transfer.
MQ135_DANGER_RATIO = 5.0

# --- Status thresholds ----------------------------------------------------

# Our choice, not from a source - thresholds for the Fresh/Use soon/Past
# use-by status bands.
STATUS_USE_SOON_PCT = 70.0
STATUS_PAST_DUE_PCT = 100.0

# Bump this any time a value above changes, so every predictions.csv row
# can be traced back to the exact parameters that produced it.
CONFIG_VERSION = "v1"
