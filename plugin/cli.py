"""``hermes gadget ...`` — inspect and manage gadgets from the Hermes host."""

from __future__ import annotations

import datetime as _dt
import socket
import sys
import time
from pathlib import Path
from urllib.parse import quote

# The browser installer: flashes a board over USB and gives it Wi-Fi and this Hermes's address.
INSTALLER_URL = "https://adolanium.github.io/hermes-gadget-sdk/"
PAIR_TIMEOUT_S = 300


def _store():
    from plugins.plugin_storage import plugin_data_dir

    from .store import DeviceStore

    return DeviceStore(plugin_data_dir("gadget"))


def _pairing_store():
    from gateway.pairing import PairingStore

    return PairingStore()


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


def device_url(extra: dict) -> str:
    """The URL devices on this network should use, from ``platforms.gadget.extra``."""
    host = extra.get("host") or "0.0.0.0"
    port = extra.get("port") or 8765
    path = "/" + str(extra.get("path") or "/gadget").strip("/")
    scheme = "wss" if extra.get("tls_cert") else "ws"
    shown = _lan_address() if host in ("0.0.0.0", "::", "") else host
    return f"{scheme}://{shown}:{port}{path}"


def installer_link(server: str) -> str:
    """The installer with this Hermes's address filled in. It rides in the fragment, which browsers
    never send to the web host."""
    return f"{INSTALLER_URL}#server={quote(server, safe='')}"


def _cmd_devices(args) -> None:
    devices = _store().devices()
    if not devices:
        print("No gadgets have connected yet.")
        return
    for device_id, rec in sorted(devices.items(), key=lambda kv: kv[1].get("name") or kv[0]):
        seen = rec.get("last_seen")
        when = _dt.datetime.fromtimestamp(seen).strftime("%Y-%m-%d %H:%M") if seen else "-"
        print(f"{device_id}  {rec.get('name') or '-':<24} {rec.get('board') or '-':<28} last seen {when}")
    print("\nApprove a new device: hermes gadget pair   |   Approved devices: hermes pairing list   |   "
          "Revoke: hermes pairing revoke gadget <device_id>")


def _match(store, ref: str):
    """The one enrolled device whose id or name is ``ref``, or None (after saying so)."""
    matches = [d for d, r in store.devices().items() if d == ref or (r.get("name") or "").lower() == ref.lower()]
    if len(matches) != 1:
        print(f"No single gadget matches {ref!r}. See: hermes gadget devices")
        return None
    return matches[0]


def _cmd_forget(args) -> None:
    store = _store()
    device_id = _match(store, args.device)
    if device_id is None:
        return
    store.forget(device_id)
    print(f"Forgot the key for {device_id}; the device will re-enroll on its next connection.")
    print(f"To also remove its chat approval: hermes pairing revoke gadget {device_id}")


def _cmd_update(args) -> None:
    from .ota import UpdateError, UpdateQueue, inspect_image

    path = Path(args.image)
    try:
        image = inspect_image(path.read_bytes())
    except OSError as exc:
        sys.exit(f"Can't read {path}: {exc}")
    except UpdateError as exc:
        sys.exit(f"{path} can't be installed: {exc.message}")
    store = _store()
    device_id = _match(store, args.device)
    if device_id is None:
        sys.exit(1)
    rec = store.devices()[device_id]
    name, board = rec.get("name") or device_id, rec.get("board")
    if image.board and board and image.board != board:
        sys.exit(f"{path} is built for {image.board}, but {name} is {board}.")

    queue = UpdateQueue(store.path.parent)
    queue.stage(device_id, image)
    print(f"Staged firmware {image.version} ({image.size // 1024} KB) for {name} ({device_id}).")
    if args.no_wait:
        print("The gateway installs it when the device is online.")
        return
    print("Waiting for the gateway to install it. Ctrl+C stops waiting; the update stays staged.")
    deadline, last = time.monotonic() + args.timeout, None
    while time.monotonic() < deadline:
        status = queue.status(device_id) or {}
        state = status.get("state")
        line = {
            "sending": f"  sending: {status.get('sent', 0) * 100 // max(1, status.get('size', 1))}%",
            "retrying": f"  {status.get('error')}; trying again when the device is back",
        }.get(state)
        if line and line != last:
            print(line)
            last = line
        if state == "done":
            print(f"Installed {status.get('version') or image.version}. {name} restarts into it and keeps it "
                  "once it reaches Hermes again.")
            return
        if state == "failed":
            sys.exit(f"The update failed: {status.get('error')}")
        time.sleep(1)
    print("Still waiting. The update stays staged and installs once the gateway sees the device online.")


def _cmd_info(args) -> None:
    extra = _gadget_extra()
    url = device_url(extra)
    print(f"platform enabled : {extra.get('enabled', False)}")
    print(f"device URL       : {url}")
    print(f"device registry  : {_store().path}")
    if not extra.get("enabled"):
        print("\nEnable it with:  hermes gateway setup   (or: hermes config set platforms.gadget.enabled true, "
              "then restart the gateway)")
    print(f"\nSet up a device from Chrome or Edge:  {installer_link(url)}")
    print(f"Or on the device's serial console:     set server {url}")


def _waiting_devices(approvals) -> list[tuple[str, dict, str]]:
    """Devices showing a pairing code that Hermes hasn't approved yet: (device id, record, code).

    The gateway records each code as Hermes issues it; a fresh store is read on every call because
    the gateway writes it from another process.
    """
    store = _store()
    waiting = []
    for device_id, rec in sorted(store.devices().items()):
        pending = store.pairing_for(device_id)
        if pending and not approvals.is_approved("gadget", device_id):
            waiting.append((device_id, rec, pending[0]))
    return waiting


def _confirm(question: str) -> bool:
    try:
        return input(f"{question} [Y/n] ").strip().lower() in ("", "y", "yes")
    except EOFError:
        return False


def _cmd_pair(args) -> None:
    approvals = _pairing_store()
    deadline = time.monotonic() + args.timeout
    announced = False
    while True:
        waiting = _waiting_devices(approvals)
        if waiting:
            break
        if time.monotonic() >= deadline:
            sys.exit("No gadget asked to pair. Check that it shows a pairing code and that the gateway is "
                     "running (hermes gadget info).")
        if not announced:
            print("Waiting for a gadget to show a pairing code. Ctrl+C stops waiting.")
            announced = True
        time.sleep(1)

    approved = 0
    for device_id, rec, code in waiting:
        name = rec.get("name") or device_id
        label = f"{name} ({device_id}{', ' + rec['board'] if rec.get('board') else ''})"
        if not args.yes and not _confirm(f"Approve {label}, showing code {code}?"):
            print(f"Left {name} unapproved.")
            continue
        if approvals.approve_code("gadget", code) is None:
            print(f"Hermes didn't accept the code for {name}: it may have expired. The device gets a new one "
                  "when it reconnects.")
            continue
        print(f"Approved {name}. It shows Ready within a few seconds.")
        approved += 1
    if not approved:
        sys.exit(1)


def setup_argparse(parser) -> None:
    subs = parser.add_subparsers(dest="gadget_command")
    subs.add_parser("devices", aliases=["ls"], help="List gadgets that have enrolled with this Hermes")
    forget = subs.add_parser("forget", help="Forget a gadget's key so it can re-enroll (after a factory reset)")
    forget.add_argument("device", help="Device id or name")
    subs.add_parser("info", help="Show the URL devices should connect to, and the installer link")
    pair = subs.add_parser("pair", help="Approve a gadget that shows a pairing code")
    pair.add_argument("--yes", "-y", action="store_true", help="Approve without asking")
    pair.add_argument("--timeout", type=float, default=PAIR_TIMEOUT_S,
                      help=f"Seconds to wait for a gadget to ask (default {PAIR_TIMEOUT_S})")
    update = subs.add_parser("update", help="Install new firmware on a gadget over the air")
    update.add_argument("device", help="Device id or name")
    update.add_argument("image", help="The firmware image: firmware.bin from the PlatformIO build")
    update.add_argument("--no-wait", action="store_true", help="Stage it and return; the gateway installs it")
    update.add_argument("--timeout", type=float, default=600, help="Seconds to wait for the install (default 600)")
    parser.set_defaults(func=handle)


_COMMANDS = {"devices": _cmd_devices, "ls": _cmd_devices, "forget": _cmd_forget, "info": _cmd_info,
             "pair": _cmd_pair, "update": _cmd_update}


def handle(args) -> None:
    command = _COMMANDS.get(getattr(args, "gadget_command", None) or "info")
    command(args)
