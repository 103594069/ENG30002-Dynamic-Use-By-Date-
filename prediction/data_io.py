"""Reading and writing the CSVs the prediction pipeline runs on.

All I/O lives here so q10_model.py and pipeline.py stay pure/testable.
Two input schemas are supported because the real Google Sheet doesn't
exist yet (see PIPELINE_PLAN.md): the HTTPS payload schema the firmware
already sends (load_https_payload_csv) and the on-device DATA.CSV
schema as a backup (load_sd_datacsv, Task 6). When the real Sheet
exists, it should be fetched the same way prediction's sibling script
`read-data-from-google-sheet` already does (public CSV export URL) and
fed through load_https_payload_csv unchanged, since the Apps Script's
actual column layout is still unverified (PIPELINE_PLAN.md, open risk
#1) - see SOURCES.md "Open questions".
"""
from __future__ import annotations

import csv
import os
from datetime import datetime
from typing import Optional

from prediction.models import PredictionRow, Reading


def _parse_float(value: Optional[str]) -> Optional[float]:
    # `value` can be None, not just "": csv.DictReader fills missing
    # trailing fields with None when a row has fewer columns than the
    # header (a hand-edited CSV, a half-written SD card row).
    if not value:
        return None
    value = value.strip()
    if not value:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def _parse_ts(value: Optional[str]) -> Optional[datetime]:
    # See _parse_float on why None is a real input here.
    if not value:
        return None
    value = value.strip()
    if not value:
        return None
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        return None


def _parse_sheet_export_ts(value: Optional[str]) -> Optional[datetime]:
    # Google Sheets' CSV export writes timestamps in US locale
    # (M/D/YYYY H:MM:SS), not ISO8601 - a separate format from the other
    # two loaders' _parse_ts.
    if not value:
        return None
    value = value.strip()
    if not value:
        return None
    try:
        return datetime.strptime(value, "%m/%d/%Y %H:%M:%S")
    except ValueError:
        return None


def _row_value(row: list[str], index: int) -> Optional[str]:
    # Mirrors csv.DictReader's "missing trailing fields -> None" behaviour
    # for the positional parsing load_sheet_export_csv needs (its 5th
    # column has no header name to key a DictReader off).
    return row[index] if index < len(row) else None


def load_https_payload_csv(path: str) -> list[Reading]:
    """Load readings logged in the HTTPS-payload schema (mock Sheet).

    Expected header (order doesn't matter, matched by name):
    server_ts, device_ts, trial, source, temp, humidity, pressure, lux,
    mq135, gas_base, gas_increase, risk.

    Raises ValueError naming the file, row, and offending value if
    `server_ts` is missing, blank, or not an ISO8601 timestamp.
    `Reading.server_ts` is the one non-Optional field, so letting a None
    through here would break that contract and fail much later (in a
    min() over timestamps, say) far from the real cause.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        # start=2 so the number matches the line in the file: row 1 is the header.
        for row_number, row in enumerate(csv.DictReader(f), start=2):
            raw_server_ts = row.get("server_ts")
            server_ts = _parse_ts(raw_server_ts)
            if server_ts is None:
                raise ValueError(
                    f"{path}: row {row_number} has an invalid or missing "
                    f"server_ts: {raw_server_ts!r}"
                )
            readings.append(Reading(
                server_ts=server_ts,
                device_ts=_parse_ts(row.get("device_ts")),
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp")),
                humidity_pct=_parse_float(row.get("humidity")),
                pressure_hpa=_parse_float(row.get("pressure")),
                lux=_parse_float(row.get("lux")),
                mq135_raw=_parse_float(row.get("mq135")),
                gas_base=_parse_float(row.get("gas_base")),
                gas_increase=_parse_float(row.get("gas_increase")),
                risk=row.get("risk") or None,
            ))
    return readings


def load_sd_datacsv(path: str) -> list[Reading]:
    """Load readings from the on-device DATA.CSV schema (backup path).

    Expected header: date,time,trial,temp_C,humidity_pct,pressure_hPa,
    lux,mq135_raw,mq135_base,mq135_increase,source. There is no
    device_ts or risk column in this schema, so those fields are None.

    Raises ValueError naming the file, row, and offending date/time values
    if the two cannot be combined into a timestamp - a bare strptime
    error gives no clue which row of which file was at fault.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        # start=2 so the number matches the line in the file: row 1 is the header.
        for row_number, row in enumerate(csv.DictReader(f), start=2):
            raw_date, raw_time = row.get("date"), row.get("time")
            try:
                server_ts = datetime.strptime(
                    f"{raw_date} {raw_time}", "%Y-%m-%d %H:%M:%S"
                )
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"{path}: row {row_number} has an invalid or missing "
                    f"date/time: date={raw_date!r} time={raw_time!r}"
                ) from exc
            readings.append(Reading(
                server_ts=server_ts,
                device_ts=None,
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp_C")),
                humidity_pct=_parse_float(row.get("humidity_pct")),
                pressure_hpa=_parse_float(row.get("pressure_hPa")),
                lux=_parse_float(row.get("lux")),
                mq135_raw=_parse_float(row.get("mq135_raw")),
                gas_base=_parse_float(row.get("mq135_base")),
                gas_increase=_parse_float(row.get("mq135_increase")),
                risk=None,
            ))
    return readings


def load_sheet_export_csv(path: str) -> list[Reading]:
    """Load readings from the real Google Sheet's CSV export.

    Expected column ORDER - matched by position, not name, because the
    real export's 5th column has no header text at all: Timestamp,
    MQ135, Temp, Humidity, Pressure. Timestamp is Google Sheets' US
    locale format (M/D/YYYY H:MM:SS), not ISO8601. This schema has no
    trial/source/device_ts/lux/gas_base/gas_increase/risk columns, so
    those fields are always None - narrower than load_https_payload_csv.

    Raises ValueError naming the file, row, and offending value if
    Timestamp is missing, blank, or unparseable, for the same reason
    load_https_payload_csv guards server_ts.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        for row_number, row in enumerate(reader, start=2):
            raw_ts = _row_value(row, 0)
            server_ts = _parse_sheet_export_ts(raw_ts)
            if server_ts is None:
                raise ValueError(
                    f"{path}: row {row_number} has an invalid or missing "
                    f"Timestamp: {raw_ts!r}"
                )
            readings.append(Reading(
                server_ts=server_ts,
                device_ts=None,
                trial=None,
                source=None,
                temp_c=_parse_float(_row_value(row, 2)),
                humidity_pct=_parse_float(_row_value(row, 3)),
                pressure_hpa=_parse_float(_row_value(row, 4)),
                lux=None,
                mq135_raw=_parse_float(_row_value(row, 1)),
                gas_base=None,
                gas_increase=None,
                risk=None,
            ))
    return readings


_PREDICTION_FIELDS = [
    "run_ts", "equivalent_hours", "pct_shelf_life_used", "remaining_hours",
    "predicted_use_by", "predicted_use_by_earliest", "predicted_use_by_latest",
    "status", "mq135_gas_increase", "mq135_disagrees", "gap_count",
    "dropped_reading_count", "q10_range_exceeded_count", "t_ref_c", "l_ref_hours",
    "q10", "q10_band_low", "q10_band_high", "forecast_window_hours", "config_version",
]


def _prediction_row_values(row: PredictionRow) -> list:
    return [
        row.run_ts.isoformat(),
        row.equivalent_hours,
        row.pct_shelf_life_used,
        row.remaining_hours,
        row.predicted_use_by.isoformat(),
        row.predicted_use_by_earliest.isoformat(),
        row.predicted_use_by_latest.isoformat(),
        row.status,
        row.mq135_gas_increase,
        row.mq135_disagrees,
        row.gap_count,
        row.dropped_reading_count,
        row.q10_range_exceeded_count,
        row.t_ref_c,
        row.l_ref_hours,
        row.q10,
        row.q10_band_low,
        row.q10_band_high,
        row.forecast_window_hours,
        row.config_version,
    ]


def write_prediction_row(path: str, row: PredictionRow) -> None:
    """Append one prediction to the predictions CSV, writing a header first."""
    is_new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if is_new:
            writer.writerow(_PREDICTION_FIELDS)
        writer.writerow(_prediction_row_values(row))


def append_daily_prediction(path: str, row: PredictionRow) -> bool:
    """Append `row` to a once-per-day predictions CSV - the mock of the
    Sheet's "Predictions" tab, one row per calendar day rather than one
    per raw reading.

    Idempotent against being re-run the same day (e.g. a daily trigger
    firing twice, or a manual re-run): if the last row already written
    has the same `run_ts.date()` as `row`, nothing is written. Returns
    True if a row was written, False if skipped as a same-day duplicate.
    """
    is_new = not os.path.exists(path)
    if not is_new:
        with open(path, newline="", encoding="utf-8") as f:
            last = None
            for last in csv.DictReader(f):
                pass
        if last is not None and datetime.fromisoformat(last["run_ts"]).date() == row.run_ts.date():
            return False
    with open(path, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if is_new:
            writer.writerow(_PREDICTION_FIELDS)
        writer.writerow(_prediction_row_values(row))
    return True
