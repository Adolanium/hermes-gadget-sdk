"""Diagnostics reports taken through the hub, without a USB cable.

The gateway asks a connected device for its report (the ``diag`` message) and adds the plugin's own
recent log, so a bug report carries both sides of the same minutes. The file has the format
``hermes-gadget diag`` and the browser installer save (python/hermes_gadget/cli.py, cmd_diag, and
site/src/lib/diagnostics.js), with the hub's log as a last section.
"""

from __future__ import annotations

import collections
import json
import logging
import re
import time

HOST_LOG_LINES = 500
# The plugin's modules log under their package's name (gateway-loaded) and hub.py under hermes_gadget.
_LOGGERS = (__name__.rpartition(".")[0] or __name__, "hermes_gadget")


class _Ring(logging.Handler):
    def __init__(self, size: int):
        super().__init__(logging.DEBUG)
        self.lines: collections.deque[str] = collections.deque(maxlen=size)
        self.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))

    def emit(self, record: logging.LogRecord) -> None:
        try:
            self.lines.append(self.format(record))
        except Exception:
            self.handleError(record)


_ring: _Ring | None = None


def keep_host_log(size: int = HOST_LOG_LINES) -> None:
    """Start keeping the plugin's recent log lines in memory. Calling it again changes nothing."""
    global _ring
    if _ring is not None:
        return
    _ring = _Ring(size)
    for name in dict.fromkeys(_LOGGERS):
        logging.getLogger(name).addHandler(_ring)


def host_log() -> list[str]:
    """The plugin's recent log lines, oldest first; empty before keep_host_log()."""
    return list(_ring.lines) if _ring is not None else []


def file_name(report: dict, when: float | None = None) -> str:
    """hermes-gadget-diag-<device>-<YYYYMMDD-HHMMSS>.txt"""
    device = re.sub(r"[^A-Za-z0-9_.-]", "_", str(report.get("device_id") or "device"))
    return f"hermes-gadget-diag-{device}-{time.strftime('%Y%m%d-%H%M%S', time.localtime(when))}.txt"


def report_text(report: dict, log: str, hub_log: list[str], when: float | None = None) -> str:
    """The file's text: where and when it was taken, the device's report and log, then the hub's log."""
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(when))
    device_log = log.rstrip("\n")
    return (f"Hermes Gadget diagnostics through the Hermes gateway, {stamp}\n\n"
            f"== report ==\n{json.dumps(report, indent=2)}\n\n"
            f"== recent log ==\n{device_log}\n\n"
            "== hub log ==\n" + "\n".join(hub_log) + "\n")
