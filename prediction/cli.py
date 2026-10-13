"""Command-line entry point: predict a use-by date from a readings CSV.

Run once or twice a day (cron on Linux/macOS, Task Scheduler on
Windows) - see README.md "Scheduling" for exact commands. Example:

    python -m prediction.cli --readings sheet_export.csv --trial ChickenTrial1
"""
from __future__ import annotations

import argparse
from datetime import datetime
from typing import Optional

from prediction import config
from prediction.data_io import (
    append_daily_prediction,
    load_https_payload_csv,
    load_sd_datacsv,
    load_sheet_export_csv,
    write_prediction_row,
)
from prediction.pipeline import q10_discrepancy_message, resolve_food_start, run_prediction

_LOADERS = {
    "https": load_https_payload_csv,
    "datacsv": load_sd_datacsv,
    "sheetexport": load_sheet_export_csv,
}


def main(argv: Optional[list[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="Predict a use-by date from logged sensor readings.")
    parser.add_argument("--readings", required=True, help="path to the readings CSV")
    parser.add_argument(
        "--format", choices=["https", "datacsv", "sheetexport"], default="https",
        help=(
            "https = mock HTTPS-payload schema (default), datacsv = SD card "
            "DATA.CSV schema, sheetexport = the real Sheet's actual CSV "
            "export schema (Timestamp, MQ135, Temp, Humidity, Pressure)"
        ),
    )
    parser.add_argument("--predictions-out", default="predictions.csv", help="CSV to append the result to")
    parser.add_argument(
        "--once-per-day", action="store_true",
        help=(
            "write to --predictions-out through the once-per-day gate "
            "(skips the write if a prediction for today was already "
            "recorded) instead of appending unconditionally"
        ),
    )
    parser.add_argument("--trial", default=None, help="only use readings for this trial name")
    parser.add_argument(
        "--food-start", default=None,
        help="ISO8601 override for when the food went in (else config.FOOD_START_TS or earliest reading)",
    )
    parser.add_argument("--now", default=None, help="ISO8601 override for 'now', for testing")
    args = parser.parse_args(argv)

    warning = q10_discrepancy_message()
    if warning:
        print(warning)

    readings = _LOADERS[args.format](args.readings)
    if args.trial:
        readings = [r for r in readings if r.trial == args.trial]

    now = datetime.fromisoformat(args.now) if args.now else datetime.now()
    configured_start = (
        datetime.fromisoformat(args.food_start) if args.food_start else config.FOOD_START_TS
    )
    # `readings` is already filtered to args.trial above, so the trial is
    # not passed again here - resolve_food_start would just re-apply the
    # same filter to an already-filtered list.
    start = resolve_food_start(readings, configured_start, trial=None)
    readings = sorted((r for r in readings if r.server_ts >= start), key=lambda r: r.server_ts)

    row = run_prediction(readings, now)
    if args.once_per_day:
        if not append_daily_prediction(args.predictions_out, row):
            print(f"Skipped: a prediction for {row.run_ts.date()} was already recorded.")
    else:
        write_prediction_row(args.predictions_out, row)

    print(f"Status: {row.status}")
    print(
        f"Equivalent hours: {row.equivalent_hours:.1f} / {row.l_ref_hours:.0f} "
        f"({row.pct_shelf_life_used:.0f}% of shelf life used)"
    )
    print(f"Remaining hours: {row.remaining_hours:.1f}")
    print(
        f"Predicted use-by: {row.predicted_use_by.isoformat()} "
        f"(range {row.predicted_use_by_earliest.isoformat()} to "
        f"{row.predicted_use_by_latest.isoformat()})"
    )
    print(f"MQ-135 gas increase: {row.mq135_gas_increase}")
    if row.mq135_disagrees:
        print("WARNING: MQ-135 disagrees with the model's Fresh status.")
    if row.gap_count:
        print(f"NOTE: {row.gap_count} reading gap(s) longer than {config.MAX_DT_HOURS}h were capped.")
    if row.dropped_reading_count:
        print(f"NOTE: {row.dropped_reading_count} reading(s) dropped for an implausible temperature.")
    if row.q10_range_exceeded_count:
        print(
            f"NOTE: {row.q10_range_exceeded_count} reading(s) fell outside the "
            f"Q10-supported temperature range {config.Q10_VALID_TEMP_RANGE_C} - "
            f"those intervals are extrapolated, not verified."
        )


if __name__ == "__main__":
    main()
