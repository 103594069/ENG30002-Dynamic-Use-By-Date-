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


_PREDICTION_FIELDS = [
    "run_ts", "equivalent_hours", "pct_shelf_life_used", "remaining_hours",
    "predicted_use_by", "predicted_use_by_earliest", "predicted_use_by_latest",
    "status", "mq135_gas_increase", "mq135_disagrees", "gap_count",
    "dropped_reading_count", "q10_range_exceeded_count", "t_ref_c", "l_ref_hours",
    "q10", "q10_band_low", "q10_band_high", "forecast_window_hours", "config_version",
]


def write_prediction_row(path: str, row: PredictionRow) -> None:
    """Append one prediction to the predictions CSV, writing a header first."""
    is_new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if is_new:
            writer.writerow(_PREDICTION_FIELDS)
        writer.writerow([
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
        ])
