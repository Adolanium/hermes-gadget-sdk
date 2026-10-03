"""``hermes gadget ...`` — inspect and manage gadgets from the Hermes host."""

from __future__ import annotations

import datetime as _dt
import socket


def _store():
    from plugins.plugin_storage import plugin_data_dir

    from .store import DeviceStore

    return DeviceStore(plugin_data_dir("gadget"))


def _lan_address() -> str:
    # Routing-table probe; nothing is sent.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def _gadget_extra() -> dict:
    try:
        from hermes_cli.config import load_config

        platforms = (load_config() or {}).get("platforms") or {}
        cfg = platforms.get("gadget") or {}
        return {"enabled": bool(cfg.get("enabled")), **(cfg.get("extra") or {})}
    except Exception:
        return {}


def _cmd_devices(args) -> None:
    devices = _store().devices()
    if not devices:
        print("No gadgets have connected yet.")
        return
    for device_id, rec in sorted(devices.items(), key=lambda kv: kv[1].get("name") or kv[0]):
        seen = rec.get("last_seen")
        when = _dt.datetime.fromtimestamp(seen).strftime("%Y-%m-%d %H:%M") if seen else "-"
        print(f"{device_id}  {rec.get('name') or '-':<24} {rec.get('board') or '-':<28} last seen {when}")
    print("\nApproved devices: hermes pairing list   |   Revoke: hermes pairing revoke gadget <device_id>")


def _cmd_forget(args) -> None:
    store = _store()
    ref = args.device
    matches = [d for d, r in store.devices().items() if d == ref or (r.get("name") or "").lower() == ref.lower()]
    if len(matches) != 1:
        print(f"No single gadget matches {ref!r}. See: hermes gadget devices")
        return
    store.forget(matches[0])
    print(f"Forgot the key for {matches[0]}; the device will re-enroll on its next connection.")
    print(f"To also remove its chat approval: hermes pairing revoke gadget {matches[0]}")


def _cmd_info(args) -> None:
    extra = _gadget_extra()
    host = extra.get("host") or "0.0.0.0"
    port = extra.get("port") or 8765
    path = "/" + str(extra.get("path") or "/gadget").strip("/")
    scheme = "wss" if extra.get("tls_cert") else "ws"
    shown = _lan_address() if host in ("0.0.0.0", "::", "") else host
    print(f"platform enabled : {extra.get('enabled', False)}")
    print(f"device URL       : {scheme}://{shown}:{port}{path}")
    print(f"device registry  : {_store().path}")
    if not extra.get("enabled"):
        print("\nEnable it with:  hermes config set platforms.gadget.enabled true   (then restart the gateway)")
    print("\nOn the device's serial console:  set server " + f"{scheme}://{shown}:{port}{path}")


def setup_argparse(parser) -> None:
    subs = parser.add_subparsers(dest="gadget_command")
    subs.add_parser("devices", aliases=["ls"], help="List gadgets that have enrolled with this Hermes")
    forget = subs.add_parser("forget", help="Forget a gadget's key so it can re-enroll (after a factory reset)")
    forget.add_argument("device", help="Device id or name")
    subs.add_parser("info", help="Show the URL devices should connect to")
    parser.set_defaults(func=handle)


_COMMANDS = {"devices": _cmd_devices, "ls": _cmd_devices, "forget": _cmd_forget, "info": _cmd_info}


def handle(args) -> None:
    command = _COMMANDS.get(getattr(args, "gadget_command", None) or "info")
    command(args)
