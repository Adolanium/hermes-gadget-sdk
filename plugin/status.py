"""The gateway's live device status, as a file other processes can read.

The device store (``devices.json``) says which devices exist and when each was
last seen. What only the running gateway knows, which devices are connected now
and their latest battery readings, goes in ``status.json`` next to it. The
gateway rewrites the file when that changes and at least every ``REFRESH_S``
seconds, so a reader can tell a gateway that stopped without saying so (a
crash, a kill) from one with nothing new to report.

Every write replaces the whole file in one ``os.replace``, so a reader in
another process (the Hermes dashboard's Gadget tab) never sees half of it.
"""

from __future__ import annotations

import json
import math
import os
import tempfile
import time
from pathlib import Path
from typing import Any

FILENAME = "status.json"
VERSION = 1
REFRESH_S = 30.0  # the gateway rewrites the file at least this often while it runs
STALE_AFTER_S = 3 * REFRESH_S  # older than this, the gateway that wrote it is taken as gone

# The readings a device's state telemetry carries about its power (firmware app_power.cpp).
BATTERY_FIELDS = ("battery_percent", "battery_mv", "charging", "external_power", "battery_present")


def battery(sensors: dict[str, Any]) -> dict[str, Any] | None:
    """The power readings among a device's sensors, or None when it reports none."""
    # A device's own readings, unpaired ones too: NaN or Infinity (which json.loads accepts) would
    # make the dashboard's JSON response fail for every device, so only finite numbers are kept.
    def finite(value: Any) -> bool:
        return isinstance(value, bool) or (isinstance(value, (int, float)) and math.isfinite(value))

    out = {k: sensors[k] for k in BATTERY_FIELDS if finite(sensors.get(k))}
    if "battery_percent" not in out and finite(sensors.get("battery_pct")) and not isinstance(sensors["battery_pct"], bool):
        out["battery_percent"] = sensors["battery_pct"]  # the protocol guide's example sensor name
    for flag in ("charging", "external_power", "battery_present"):
        if flag in out:
            out[flag] = bool(out[flag])  # firmware sends some flags as 0/1
    return out or None


def write(directory: Path | str, devices: dict[str, dict[str, Any]], *, running: bool = True,
          now: float | None = None) -> None:
    """Replace the status file with ``devices`` (device id -> its live state)."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    data = {
        "version": VERSION,
        "running": running,
        "pid": os.getpid(),
        "updated_at": time.time() if now is None else now,
        "refresh_s": REFRESH_S,
        "devices": devices,
    }
    path = directory / FILENAME
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=f"{path.stem}.", suffix=".tmp")  # unique, mode 600
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(json.dumps(data, indent=2, sort_keys=True, allow_nan=False))
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def read(directory: Path | str, *, now: float | None = None) -> dict[str, Any]:
    """What the gateway last wrote: ``running`` (it is up and the file is fresh), ``updated_at`` and
    ``devices``. A missing, unreadable, stopped or stale file reads as not running, with no devices."""
    now = time.time() if now is None else now
    try:
        raw = json.loads((Path(directory) / FILENAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raw = None
    if not isinstance(raw, dict):
        return {"running": False, "updated_at": None, "devices": {}}
    updated_at = raw.get("updated_at")
    updated_at = float(updated_at) if isinstance(updated_at, (int, float)) else None
    fresh = updated_at is not None and now - updated_at <= STALE_AFTER_S
    running = bool(raw.get("running")) and fresh
    devices = raw.get("devices") if running and isinstance(raw.get("devices"), dict) else {}
    return {"running": running, "updated_at": updated_at,
            "devices": {k: v for k, v in devices.items() if isinstance(v, dict)}}
