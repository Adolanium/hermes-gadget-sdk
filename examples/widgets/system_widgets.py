#!/usr/bin/env python3
"""Feed a gadget's home-screen widgets with this computer's state.

Sets three widgets with ``hermes gadget widget set``: how full a disk is (a
progress bar that turns yellow at 80% and red at 90%), the load average (a big
value, where the system reports one) and a headline with the host name and the
date. Each expires after ``--ttl`` seconds, so a job that stops running leaves
no stale numbers on the screen.

Run it from cron on the Hermes computer, for example every five minutes:

    */5 * * * * python3 /path/to/examples/widgets/system_widgets.py

It needs only Python's standard library and the ``hermes`` command on PATH.
``--dry-run`` prints the commands instead of running them.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import socket
import subprocess
import sys
import time


def commands(disk: str = "/", ttl: int = 900, device: str | None = None) -> list[list[str]]:
    """The ``hermes gadget widget set`` commands for this computer's state right now."""
    tail = ["--ttl", str(ttl)] + (["--device", device] if device else [])
    out = []

    usage = shutil.disk_usage(disk)
    percent = round(100 * usage.used / usage.total)
    state = "bad" if percent >= 90 else "warn" if percent >= 80 else "ok"
    out.append(["disk", "--percent", str(percent), "--label", f"Disk {disk}", "--state", state])

    if hasattr(os, "getloadavg"):  # not on Windows
        load = os.getloadavg()[0]
        cores = os.cpu_count() or 1
        state = "bad" if load > 2 * cores else "warn" if load > cores else "ok"
        out.append(["load", "--value", f"{load:.2f}", "--label", "Load", "--state", state])

    out.append(["host", "--title", socket.gethostname(), "--text", time.strftime("%a %d %b"), "--label", "Hermes host"])
    return [["hermes", "gadget", "widget", "set", *cmd, *tail] for cmd in out]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--disk", default=os.path.abspath(os.sep), help="The disk to report (default: the root)")
    parser.add_argument("--ttl", type=int, default=900, help="Seconds each widget lives unless set again (default 900)")
    parser.add_argument("--device", help="Only this gadget (id or name); every gadget without it")
    parser.add_argument("--dry-run", action="store_true", help="Print the commands instead of running them")
    args = parser.parse_args(argv)
    for cmd in commands(args.disk, args.ttl, args.device):
        if args.dry_run:
            print(shlex.join(cmd))
        elif subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
