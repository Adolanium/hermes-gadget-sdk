"""``hermes-gadget`` command line."""

from __future__ import annotations

import argparse
import asyncio
import logging
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import __version__, paths


# -- sim ------------------------------------------------------------------------------------

def _make_sim(args):
    from .sim import Simulator

    state = Path(args.state_dir) if args.state_dir else paths.default_state_dir() / "sim" / args.name.replace(" ", "-").lower()
    return Simulator(url=args.url or "", board=args.board, name=args.name, token=args.token or "",
                     state_dir=state, live_audio=args.live_audio)


def cmd_sim(args) -> int:
    from .sim.native import SimLibraryError

    try:
        sim = _make_sim(args)
    except SimLibraryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.headless:
        return run_script(sim, Path(args.script) if args.script else None)
    from .sim.window import SimulatorWindow

    SimulatorWindow(sim, zoom=args.zoom).run()
    return 0


class ScriptError(RuntimeError):
    pass


def run_script(sim, script: Path | None) -> int:
    """Headless driver: one command per line (see docs/simulator.md)."""
    lines = script.read_text(encoding="utf-8").splitlines() if script else ["wait ready 20", "status"]
    sim.start(network=True)
    try:
        for number, raw in enumerate(lines, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            cmd, _, rest = line.partition(" ")
            try:
                _script_step(sim, cmd, rest.strip())
            except ScriptError as exc:
                print(f"{script or '<default>'}:{number}: {exc}", file=sys.stderr)
                return 1
        return 0
    finally:
        sim.close()


def _script_step(sim, cmd: str, rest: str) -> None:
    if cmd == "wait":
        parts = rest.split()
        if not parts:
            raise ScriptError("wait needs a screen name")
        timeout = float(parts[1]) if len(parts) > 1 else 15.0
        if not sim.wait_screen(*parts[0].split("|"), timeout=timeout):
            raise ScriptError(f"timed out waiting for screen {parts[0]} (now {sim.device.screen()})")
        print(f"screen {sim.device.screen()}")
    elif cmd == "text":
        sim.type_text(rest)
    elif cmd in ("press", "release", "tap"):
        getattr(sim, cmd)(rest)
    elif cmd == "wav":
        print(f"speaking {rest} ({sim.speak_wav(rest):.1f}s)")
    elif cmd == "sleep":
        sim.run_for(float(rest))
    elif cmd == "console":
        print(sim.console(rest))
    elif cmd == "status":
        print(sim.status())
    elif cmd == "screenshot":
        print(f"saved {sim.screenshot(rest)}")
    elif cmd == "expect":
        model = sim.last_received("reply") or {}
        if rest.lower() not in str(model.get("text", "")).lower():
            raise ScriptError(f"expected reply containing {rest!r}, last reply was {model.get('text')!r}")
        print(f"ok: reply contains {rest!r}")
    else:
        raise ScriptError(f"unknown command {cmd!r}")


# -- devserver ---------------------------------------------------------------------------------

def cmd_devserver(args) -> int:
    from . import devserver

    state = Path(args.state_dir) if args.state_dir else paths.default_state_dir() / "devserver"
    try:
        asyncio.run(devserver.serve(args.host, args.port, args.path, state, require_pairing=args.pairing,
                                    loopback=not args.no_loopback, token=args.token))
    except KeyboardInterrupt:
        pass
    return 0


# -- build-sim -----------------------------------------------------------------------------------

def cmd_build_sim(args) -> int:
    cmake = shutil.which("cmake")
    if not cmake:
        print("error: cmake not found on PATH", file=sys.stderr)
        return 2
    src, build = paths.firmware_dir(), paths.host_build_dir()
    configure = [cmake, "-S", str(src), "-B", str(build)]
    if args.generator:
        configure += ["-G", args.generator]
    if sys.platform != "win32":
        configure += [f"-DCMAKE_BUILD_TYPE={args.config}"]
    for step in (configure, [cmake, "--build", str(build), "--config", args.config]):
        print("+", " ".join(step))
        if subprocess.run(step).returncode != 0:
            return 1
    if args.test:
        ctest = shutil.which("ctest") or "ctest"
        if subprocess.run([ctest, "--test-dir", str(build), "-C", args.config, "--output-on-failure"]).returncode != 0:
            return 1
    lib = paths.find_sim_library()
    print(f"simulator library: {lib}")
    return 0 if lib else 1


# -- plugin install ---------------------------------------------------------------------------------

def cmd_plugin_install(args) -> int:
    home = Path(args.hermes_home) if args.hermes_home else paths.hermes_home()
    target = home / "plugins" / "gadget"
    source = paths.plugin_dir()
    if target.exists() or target.is_symlink():
        if not args.force:
            print(f"{target} exists; pass --force to replace it", file=sys.stderr)
            return 1
        if target.is_symlink() or target.is_file():
            target.unlink()
        else:
            shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if args.link:
        try:
            target.symlink_to(source, target_is_directory=True)
        except OSError as exc:
            print(f"cannot create a symlink ({exc}); copying instead")
            args.link = False
    if not args.link:
        shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    print(f"installed plugin -> {target}{' (linked)' if args.link else ''}")
    print("\nNext:")
    print("  hermes plugins enable gadget")
    print("  hermes config set platforms.gadget.enabled true")
    print("  hermes gateway run            # or restart your gateway service")
    print("  hermes gadget info            # shows the URL to give your device")
    return 0


# -- serial provisioning ----------------------------------------------------------------------------

def _serial(port: str, baud: int):
    try:
        import serial  # pyserial
    except ImportError:
        raise SystemExit("pyserial is required: pip install 'hermes-gadget[serial]'") from None
    return serial.Serial(port, baud, timeout=0.2)


def _serial_lines(ser, line: str, done, timeout: float) -> list[str]:
    """Send a console line and collect complete output lines until ``done(line)``.

    Long replies (``diag``) arrive over several reads, so a line counts only
    once its newline has arrived. Returns every line read, the last one being
    the line that satisfied ``done`` (or whatever arrived before the timeout).
    """
    ser.write((line + "\n").encode())
    end = time.monotonic() + timeout
    buf, lines = b"", []
    while time.monotonic() < end:
        buf += ser.read(512)
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            text = raw.decode(errors="replace").rstrip("\r")
            lines.append(text)
            if done(text):
                return lines
    return lines


def _serial_command(ser, line: str, timeout: float = 3.0) -> str:
    """The first machine-readable ('@') reply to a console line, or ''."""
    lines = _serial_lines(ser, line, lambda text: text.startswith("@"), timeout)
    return lines[-1] if lines and lines[-1].startswith("@") else ""


def cmd_provision(args) -> int:
    settings = [("wifi_ssid", args.wifi_ssid), ("wifi_pass", args.wifi_pass), ("server", args.server),
                ("token", args.token), ("name", args.name)]
    ser = _serial(args.port, args.baud)
    with ser:
        time.sleep(0.5)
        ser.reset_input_buffer()
        for key, value in settings:
            if value is None:
                continue
            reply = _serial_command(ser, f"set {key} {value}")
            shown = "<hidden>" if key in ("wifi_pass", "token") else value
            print(f"set {key} = {shown}: {reply or 'no answer'}")
            if not reply.startswith("@ok"):
                return 1
        print(_serial_command(ser, "status") or "no status reply")
    return 0


def _diag_summary(report: dict) -> list[str]:
    """A few lines from a ``diag`` report, for the terminal."""
    out = [f"firmware {report.get('firmware')} on {report.get('board')} ({report.get('device_id')})"]
    if "reset_reason" in report:
        out.append(f"reset reason: {report['reset_reason']}, up {report.get('uptime_s', '?')} s")
    parts = report.get("parts")
    if isinstance(parts, dict):
        out.append("parts: " + ", ".join(
            f"{k} {'yes' if v is True else 'no' if v is False else v}" for k, v in parts.items()))
    if isinstance(report.get("i2c"), list):
        out.append("I2C: " + (" ".join(report["i2c"]) or "nothing answered"))
    wifi = report.get("wifi")
    if isinstance(wifi, dict):
        out.append(f"Wi-Fi: joined {wifi.get('ssid')} ({wifi.get('rssi')} dBm), {wifi.get('ip', 'no address')}"
                   if wifi.get("joined") else "Wi-Fi: not joined")
    app, conn = report.get("app") or {}, report.get("connection") or {}
    line = f"Hermes: {app.get('phase')}, {'paired' if app.get('paired') else 'not paired'}"
    if conn.get("last_close"):
        line += f", last connection ended: {conn['last_close']}"
    if app.get("error"):
        line += f", error: {app['error']}"
    out.append(line)
    return out


def cmd_diag(args) -> int:
    import json
    import re

    ser = _serial(args.port, args.baud)
    with ser:
        time.sleep(0.5)
        ser.reset_input_buffer()
        reply = _serial_command(ser, "diag", timeout=8.0)
        if not reply.startswith("@diag "):
            print(f"No diag report from {args.port} ({reply or 'no answer'}). Is the firmware running, "
                  "and is this its console port?", file=sys.stderr)
            return 1
        report = json.loads(reply[len("@diag "):])
        log = _serial_lines(ser, "diag log", lambda text: text == "@log end" or text.startswith("@error"), 8.0)
    log = [line for line in log if line != "@log end" and not line.startswith("gadget>")]

    stamp = time.strftime("%Y%m%d-%H%M%S")
    device = re.sub(r"[^A-Za-z0-9_.-]", "_", str(report.get("device_id") or "device"))
    path = Path(args.out) if args.out else Path(f"hermes-gadget-diag-{device}-{stamp}.txt")
    path.write_text(
        f"Hermes Gadget diagnostics from {args.port}, {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n"
        f"== report ==\n{json.dumps(report, indent=2)}\n\n== recent log ==\n" + "\n".join(log) + "\n",
        encoding="utf-8")
    print("\n".join(_diag_summary(report)))
    print(f"\nSaved the full report to {path}. Attach it when you open an issue.")
    return 0


def cmd_console(args) -> int:
    ser = _serial(args.port, args.baud)
    print("Serial console - type commands (e.g. 'status', 'help'); Ctrl+C to exit.")
    import threading

    def reader():
        while ser.is_open:
            try:
                data = ser.read(256)
            except Exception:
                return
            if data:
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            ser.write(line.rstrip("\r\n").encode() + b"\n")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


# -- entry point ----------------------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="hermes-gadget", description="Hermes Gadget SDK tools")
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)

    from .sim.runner import BOARDS

    s = sub.add_parser("sim", help="Run the desktop simulator (the real device core)")
    s.add_argument("--url", help="Hermes gadget endpoint, e.g. ws://127.0.0.1:8765/gadget")
    s.add_argument("--board", default="sim-320x240", choices=sorted(BOARDS))
    s.add_argument("--name", default="Sim Gadget", help="Device name shown to Hermes")
    s.add_argument("--token", help="Access token, when the Hermes side requires one")
    s.add_argument("--state-dir", help="Where the simulated device keeps its NVS, audio and screenshots")
    s.add_argument("--live-audio", action="store_true", help="Use the PC microphone and speakers (needs sounddevice)")
    s.add_argument("--zoom", type=int, default=2)
    s.add_argument("--headless", action="store_true", help="No window; run --script (or connect and report)")
    s.add_argument("--script", help="Headless script file")
    s.set_defaults(func=cmd_sim)

    d = sub.add_parser("devserver", help="Run the device hub with a scripted brain (no Hermes needed)")
    d.add_argument("--host", default="0.0.0.0")
    d.add_argument("--port", type=int, default=8765)
    d.add_argument("--path", default="/gadget")
    d.add_argument("--pairing", action="store_true", help="Require pairing codes (approve from the console)")
    d.add_argument("--no-loopback", action="store_true", help="Do not play recorded voice back")
    d.add_argument("--token", help="Require this access token from devices")
    d.add_argument("--state-dir")
    d.set_defaults(func=cmd_devserver)

    b = sub.add_parser("build-sim", help="Build the simulator library and core tests with CMake")
    b.add_argument("--config", default="Release")
    b.add_argument("--generator", help="CMake generator override")
    b.add_argument("--test", action="store_true", help="Also run the core unit tests")
    b.set_defaults(func=cmd_build_sim)

    pl = sub.add_parser("plugin", help="Manage the Hermes plugin")
    pls = pl.add_subparsers(dest="plugin_command", required=True)
    pi = pls.add_parser("install", help="Install the gadget plugin into a Hermes home")
    pi.add_argument("--hermes-home", help="Defaults to $HERMES_HOME or ~/.hermes")
    pi.add_argument("--link", action="store_true", help="Symlink instead of copy (plugin development)")
    pi.add_argument("--force", action="store_true")
    pi.set_defaults(func=cmd_plugin_install)

    pr = sub.add_parser("provision", help="Configure a board over its serial console")
    pr.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    pr.add_argument("--baud", type=int, default=115200)
    pr.add_argument("--wifi-ssid")
    pr.add_argument("--wifi-pass")
    pr.add_argument("--server", help="ws://<hermes-host>:8765/gadget")
    pr.add_argument("--token")
    pr.add_argument("--name")
    pr.set_defaults(func=cmd_provision)

    c = sub.add_parser("console", help="Interactive serial console for a board")
    c.add_argument("--port", required=True)
    c.add_argument("--baud", type=int, default=115200)
    c.set_defaults(func=cmd_console)

    dg = sub.add_parser("diag", help="Save a board's diagnostics report (for bug reports)")
    dg.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    dg.add_argument("--baud", type=int, default=115200)
    dg.add_argument("--out", help="Report file (default: hermes-gadget-diag-<device>-<time>.txt)")
    dg.set_defaults(func=cmd_diag)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    return int(args.func(args) or 0)
