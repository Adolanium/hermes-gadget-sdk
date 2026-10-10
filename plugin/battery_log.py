"""Optional battery log: one CSV per device of its power readings over time.

Gadgets report battery readings in their state telemetry every few seconds.
With ``battery_log: true`` the adapter appends them here, so a full discharge
can be used to fit a board's voltage-to-percent curve. Off by default.
"""

from __future__ import annotations

import csv
import re
import time
from pathlib import Path
from typing import Callable, Dict, Optional, Tuple

FIELDS = ("battery_mv", "battery_percent", "charging", "external_power", "battery_present")
HEADER = ("time", "unix_s") + FIELDS
INTERVAL_S = 30.0  # a row per half minute; a change of power source is written at once
MAX_BYTES = 8 * 1024 * 1024  # per file; then it moves to <name>.1.csv, replacing the previous one


class BatteryLog:
    def __init__(self, directory: Path | str, *, clock: Callable[[], float] = time.time,
                 interval_s: float = INTERVAL_S, max_bytes: int = MAX_BYTES):
        self.directory = Path(directory)
        self._clock = clock
        self._interval_s = interval_s
        self._max_bytes = max_bytes
        self._last: Dict[str, Tuple[float, Tuple]] = {}  # device id -> (when, power source)

    def path(self, device_id: str) -> Path:
        return self.directory / f"{re.sub(r'[^A-Za-z0-9_.-]', '_', device_id)}.csv"

    def record(self, device_id: str, sensors: dict) -> Optional[Path]:
        """Appends a row when the readings include a battery voltage or percent and one is due."""
        if sensors.get("battery_mv") is None and sensors.get("battery_percent") is None:
            return None
        now = self._clock()
        source = (sensors.get("charging"), sensors.get("external_power"))
        last = self._last.get(device_id)
        if last and now - last[0] < self._interval_s and source == last[1]:
            return None
        self._last[device_id] = (now, source)
        path = self.path(device_id)
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.stat().st_size >= self._max_bytes:
            path.replace(path.with_suffix(".1.csv"))
        new = not path.exists()
        with path.open("a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(HEADER)
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(now))
            w.writerow([stamp, int(now)] + [_cell(sensors.get(k)) for k in FIELDS])
        return path


def _cell(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)
