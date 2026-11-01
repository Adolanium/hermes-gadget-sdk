"""Backend of the Hermes dashboard's Gadgets tab, mounted at ``/api/plugins/gadget/``.

The dashboard imports this file on its own, in its own process, behind its auth: it is not the
gateway, which holds the device connections. It reads the files the gateway and ``hermes gadget``
already share, in the plugin's data directory, and never calls into the gateway:

- ``devices.json`` (``store.DeviceStore``): every known device, its board, firmware and last contact;
- ``status.json`` (``status``): which devices are connected now and their battery, written by the
  running gateway.

Device keys never leave the store: only the fields listed in ``_device`` are returned.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from typing import Any

from fastapi import APIRouter

PLUGIN_DIR = Path(__file__).resolve().parent.parent
_PACKAGE = "hermes_gadget_dashboard_plugin"


def _plugin_package():
    """The plugin directory as a package, so this file uses the same store and status code as the
    gateway and the CLI from the same install. Its ``__init__`` imports nothing from Hermes."""
    module = sys.modules.get(_PACKAGE)
    if module is None:
        spec = importlib.util.spec_from_file_location(
            _PACKAGE, PLUGIN_DIR / "__init__.py", submodule_search_locations=[str(PLUGIN_DIR)])
        module = importlib.util.module_from_spec(spec)
        sys.modules[_PACKAGE] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            sys.modules.pop(_PACKAGE, None)
            raise
    return module


_plugin_package()
from hermes_gadget_dashboard_plugin import status as live_status  # noqa: E402
from hermes_gadget_dashboard_plugin.store import DeviceStore  # noqa: E402

router = APIRouter()


def _data_dir() -> Path:
    # Resolved per request: it follows the profile the dashboard request is scoped to.
    from plugins.plugin_storage import plugin_data_dir

    return plugin_data_dir("gadget")


def _device(device_id: str, rec: dict[str, Any], live: dict[str, Any] | None) -> dict[str, Any]:
    live = live or {}
    return {
        "id": device_id,
        "name": rec.get("name") or "",
        "board": rec.get("board") or "",
        "firmware": rec.get("firmware") or "",
        "last_seen": rec.get("last_seen"),
        "enrolled_at": rec.get("enrolled_at"),
        "pending": bool(rec.get("pending")),
        "online": bool(live.get("online")),
        "connected_at": live.get("connected_at"),
        "battery": live.get("battery") if isinstance(live.get("battery"), dict) else None,
    }


@router.get("/devices")
def devices() -> dict[str, Any]:
    """Every known device with its live state. ``gateway.running`` is false when no gateway has
    written a fresh status file; every device then reads as offline, with no battery."""
    directory = _data_dir()
    known = DeviceStore(directory).devices()
    live = live_status.read(directory)
    rows = [_device(k, v, live["devices"].get(k)) for k, v in known.items()]
    rows.sort(key=lambda d: ((d["name"] or d["id"]).lower(), d["id"]))
    return {
        "gateway": {"running": live["running"], "updated_at": live["updated_at"],
                    "stale_after_s": live_status.STALE_AFTER_S},
        "devices": rows,
    }
