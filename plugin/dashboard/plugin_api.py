"""Backend of the Hermes dashboard's Gadgets tab and the Hermes Desktop Gadgets page, mounted at
``/api/plugins/gadget/``.

The dashboard imports this file on its own, in its own process, behind its auth: it is not the
gateway, which holds the device connections. It reads the files the gateway and ``hermes gadget``
already share, in the plugin's data directory, and never calls into the gateway:

- ``devices.json`` (``store.DeviceStore``): every known device, its board, firmware and last contact;
- ``status.json`` (``status``): which devices are connected now and their battery, written by the
  running gateway, with what each screen shows (the Desktop page's mirrors);
- ``desk/`` (``desk.Desk``): cards and device actions the Desktop page sends, which the running
  gateway carries to the device and answers.

Pairing approval goes through Hermes's own pairing store, as ``hermes gadget pair`` does.

Device keys never leave the store: only the fields listed in ``_device`` are returned.
"""

from __future__ import annotations

import importlib.util
import os
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
from hermes_gadget_dashboard_plugin.desk import Desk  # noqa: E402
from hermes_gadget_dashboard_plugin.store import DeviceStore  # noqa: E402

_STATES = ("ready", "listening", "thinking", "speaking")

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
        "paired": bool(live.get("paired")) if live else None,
        "activity": _activity(live.get("activity")),
        "actions": [a for a in (live.get("actions") or []) if isinstance(a, str)][:16],
    }


def _activity(raw: Any) -> dict[str, Any] | None:
    """What the device's screen shows (Desktop's live mirror); only known fields, plain strings."""
    if not isinstance(raw, dict):
        return None
    state = raw.get("state") if raw.get("state") in _STATES else "ready"
    text = {k: str(raw.get(k) or "")[:280] for k in ("heard", "reply", "status")}
    at = raw.get("at") if isinstance(raw.get("at"), (int, float)) else None
    return {"state": state, **text, "at": at}


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



# -- the Hermes Desktop page ---------------------------------------------------------------------
# The page shows the same devices with what each screen is showing, approves pairing codes, and
# sends cards and actions. Pairing goes through Hermes's own pairing store, as `hermes gadget pair`
# does; cards and actions go through desk/inbox/, which the running gateway carries to the device.


def _error(message: str, status: int = 400) -> dict[str, Any]:
    return {"ok": False, "error": message, "status": status}


@router.get("/pairing")
def pairing() -> dict[str, Any]:
    """Devices showing a pairing code that Hermes hasn't approved yet."""
    from gateway.pairing import PairingStore

    store, approvals = DeviceStore(_data_dir()), PairingStore()
    waiting = []
    for device_id, rec in sorted(store.devices().items()):
        found = store.pairing_for(device_id)
        if found and not approvals.is_approved("gadget", device_id):
            waiting.append({"id": device_id, "name": rec.get("name") or device_id,
                            "board": rec.get("board") or "", "code": found[0]})
    return {"ok": True, "waiting": waiting}


@router.post("/pairing/{device_id}/approve")
def approve(device_id: str) -> dict[str, Any]:
    """Approve the code a device is showing. Only a device that is waiting can be approved."""
    from gateway.pairing import PairingStore

    store, approvals = DeviceStore(_data_dir()), PairingStore()
    found = store.pairing_for(device_id)
    if not found or approvals.is_approved("gadget", device_id):
        return _error("That gadget isn't waiting to pair. It gets a new code when it reconnects.", 404)
    if approvals.approve_code("gadget", found[0]) is None:
        return _error("Hermes didn't accept the code: it may have expired. The gadget gets a new one when it reconnects.", 409)
    return {"ok": True, "approved": device_id}


@router.post("/devices/{device_id}/card")
def card(device_id: str, title: str = "", text: str = "", seconds: str = "20") -> dict[str, Any]:
    """Show a card on a connected device's screen."""
    desk = Desk(_data_dir())
    try:
        req = desk.post(device_id, "card", title=title, text=text, seconds=seconds)
    except ValueError as exc:
        return _error(str(exc))
    return desk.wait(req, 8.0)


@router.post("/devices/{device_id}/action")
def action(device_id: str, name: str, args: str = "{}") -> dict[str, Any]:
    """Run one of the actions a connected device advertises (an LED, a relay)."""
    import json

    try:
        parsed = json.loads(args or "{}")
    except ValueError:
        return _error("args must be JSON")
    desk = Desk(_data_dir())
    try:
        req = desk.post(device_id, "action", action=name, args=parsed)
    except ValueError as exc:
        return _error(str(exc))
    return desk.wait(req, 18.0)


@router.get("/info")
def info() -> dict[str, Any]:
    """Where devices connect, for the Desktop page's virtual gadget on this computer."""
    try:
        from hermes_cli.config import load_config

        cfg = ((load_config() or {}).get("platforms") or {}).get("gadget") or {}
    except Exception:
        cfg = {}
    extra = cfg.get("extra") or {}
    try:
        port = int(extra.get("port") or 8765)
    except (TypeError, ValueError):
        port = 8765
    path = "/" + str(extra.get("path") or "/gadget").strip("/")
    scheme = "wss" if extra.get("tls_cert") else "ws"
    # The gateway listens on its configured host; a wildcard bind (the default) includes loopback.
    host = str(extra.get("host") or "").strip()
    if host in ("", "0.0.0.0", "::", "[::]", "*", "localhost"):
        host = "127.0.0.1"
    elif ":" in host and not host.startswith("["):
        host = f"[{host}]"  # an IPv6 address in a URL
    live = live_status.read(_data_dir())
    return {"ok": True, "enabled": bool(cfg.get("enabled")), "running": live["running"],
            "url": f"{scheme}://{host}:{port}{path}", "port": port, "path": path,
            "token_required": _token_required(extra)}


def _token_required(extra: dict[str, Any]) -> bool:
    """Whether the gateway asks devices for a token: ``extra.access_token`` or the
    ``GADGET_ACCESS_TOKEN`` secret, the same two places the adapter reads."""
    if str(extra.get("access_token") or "").strip():
        return True
    try:
        from agent.secret_scope import get_secret

        return bool(get_secret("GADGET_ACCESS_TOKEN"))
    except Exception:
        return bool(os.environ.get("GADGET_ACCESS_TOKEN"))
