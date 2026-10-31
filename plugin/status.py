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
import os
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
    out = {k: sensors[k] for k in BATTERY_FIELDS if isinstance(sensors.get(k), (bool, int, float))}
    if "battery_percent" not in out and isinstance(sensors.get("battery_pct"), (int, float)):
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
    tmp = path.with_name(f"{path.stem}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    os.replace(tmp, path)


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
