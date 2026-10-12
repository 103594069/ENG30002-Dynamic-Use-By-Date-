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
from datetime import datetime
from typing import Optional

from prediction.models import Reading


def _parse_float(value: str) -> Optional[float]:
    value = value.strip()
    if not value:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def _parse_ts(value: str) -> Optional[datetime]:
    value = value.strip()
    if not value:
        return None
    return datetime.fromisoformat(value)


def load_https_payload_csv(path: str) -> list[Reading]:
    """Load readings logged in the HTTPS-payload schema (mock Sheet).

    Expected header (order doesn't matter, matched by name):
    server_ts, device_ts, trial, source, temp, humidity, pressure, lux,
    mq135, gas_base, gas_increase, risk.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            readings.append(Reading(
                server_ts=_parse_ts(row["server_ts"]),
                device_ts=_parse_ts(row.get("device_ts", "")),
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp", "")),
                humidity_pct=_parse_float(row.get("humidity", "")),
                pressure_hpa=_parse_float(row.get("pressure", "")),
                lux=_parse_float(row.get("lux", "")),
                mq135_raw=_parse_float(row.get("mq135", "")),
                gas_base=_parse_float(row.get("gas_base", "")),
                gas_increase=_parse_float(row.get("gas_increase", "")),
                risk=row.get("risk") or None,
            ))
    return readings


def load_sd_datacsv(path: str) -> list[Reading]:
    """Load readings from the on-device DATA.CSV schema (backup path).

    Expected header: date,time,trial,temp_C,humidity_pct,pressure_hPa,
    lux,mq135_raw,mq135_base,mq135_increase,source. There is no
    device_ts or risk column in this schema, so those fields are None.
    """
    readings: list[Reading] = []
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            server_ts = datetime.strptime(
                f"{row['date']} {row['time']}", "%Y-%m-%d %H:%M:%S"
            )
            readings.append(Reading(
                server_ts=server_ts,
                device_ts=None,
                trial=row.get("trial") or None,
                source=row.get("source") or None,
                temp_c=_parse_float(row.get("temp_C", "")),
                humidity_pct=_parse_float(row.get("humidity_pct", "")),
                pressure_hpa=_parse_float(row.get("pressure_hPa", "")),
                lux=_parse_float(row.get("lux", "")),
                mq135_raw=_parse_float(row.get("mq135_raw", "")),
                gas_base=_parse_float(row.get("mq135_base", "")),
                gas_increase=_parse_float(row.get("mq135_increase", "")),
                risk=None,
            ))
    return readings
