"""A diagnostics report taken through the hub: the hub's request, the plugin's own log, the file
it saves, and `hermes gadget diag` (the device's side runs in test_sim_hub.py and test_app.cpp)."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import threading
import time
import types

import pytest

from hermes_gadget_plugin import cli as plugin_cli
from hermes_gadget_plugin import diag
from hermes_gadget_plugin.desk import Desk
from hermes_gadget_plugin.hub import ActionError, DeviceHub, DeviceSession
from hermes_gadget_plugin.store import DeviceStore

DEVICE = "hg-0123456789abcdef"


class _Wire:
    remote_address = ("127.0.0.1", 1)

    def __init__(self):
        self.sent: list[dict] = []

    async def send(self, raw):
        self.sent.append(json.loads(raw))


def test_the_hub_matches_the_devices_answer_to_its_request():
    wire = _Wire()
    session = DeviceSession(types.SimpleNamespace(_held={}), wire, {"device_id": DEVICE})

    async def run():
        asking = asyncio.create_task(session.request_diag(timeout=2))
        while not wire.sent:
            await asyncio.sleep(0)
        sent = wire.sent[0]
        assert sent["type"] == "diag"
        # An answer to another request, and one without a report, change nothing / default.
        await DeviceHub._h_diag(None, session, {"type": "diag", "id": "other", "report": {"x": 1}})
        await DeviceHub._h_diag(None, session, {"type": "diag", "id": sent["id"], "report": {"board": "b"}, "log": "I (1) hg: up\n"})
        return await asking

    assert asyncio.run(run()) == {"report": {"board": "b"}, "log": "I (1) hg: up\n"}
    assert not session._pending


def test_a_device_that_never_answers_says_its_firmware_may_be_old():
    session = DeviceSession(types.SimpleNamespace(_held={}), _Wire(), {"device_id": DEVICE})
    with pytest.raises(ActionError, match="too old"):
        asyncio.run(session.request_diag(timeout=0.05))
    assert not session._pending


def test_the_file_has_the_usb_reports_sections_and_the_hubs_log():
    when = time.mktime((2026, 11, 8, 14, 3, 9, 0, 0, -1))
    report = {"device_id": "hg-00/x", "board": "test"}
    text = diag.report_text(report, "I (1) hg: up\nI (2) hg: online\n", ["12:00 INFO hermes_gadget.hub: online"], when)
    assert text.startswith("Hermes Gadget diagnostics through the Hermes gateway, 2026-11-08 14:03:09\n\n")
    assert f"== report ==\n{json.dumps(report, indent=2)}\n\n" in text
    assert "== recent log ==\nI (1) hg: up\nI (2) hg: online\n\n== hub log ==\n12:00 INFO hermes_gadget.hub: online\n" in text
    assert diag.file_name(report, when) == "hermes-gadget-diag-hg-00_x-20261108-140309.txt"


def test_the_plugin_keeps_its_recent_log_lines(monkeypatch):
    monkeypatch.setattr(diag, "_ring", None)
    diag.keep_host_log(size=3)
    diag.keep_host_log(size=3)  # once only: no line twice
    try:
        log = logging.getLogger("hermes_gadget.hub")
        old = log.level
        log.setLevel(logging.INFO)
        for i in range(5):
            log.info("line %d", i)
        log.setLevel(old)
        lines = diag.host_log()
        assert [line.rsplit(" ", 1)[1] for line in lines] == ["2", "3", "4"]
        assert "INFO hermes_gadget.hub: line 4" in lines[-1]
    finally:
        for name in dict.fromkeys(diag._LOGGERS):
            logging.getLogger(name).removeHandler(diag._ring)


def _gateway_answers(directory, reply):
    desk = Desk(directory)

    def run():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for req_id, req in desk.pending():
                desk.finish(req_id, reply(req))
                return
            time.sleep(0.02)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def test_hermes_gadget_diag_saves_the_gateways_report(tmp_path, monkeypatch, capsys):
    store = DeviceStore(tmp_path)
    store.enroll(DEVICE, b"k" * 32, name="Kitchen", board="test", address="127.0.0.1")
    monkeypatch.setattr(plugin_cli, "_store", lambda: DeviceStore(tmp_path))
    out = tmp_path / "report.txt"

    thread = _gateway_answers(tmp_path, lambda req: {"ok": True, "file": "x.txt", "text": f"for {req['device']}"})
    plugin_cli.handle(argparse.Namespace(gadget_command="diag", device="kitchen", out=str(out)))
    thread.join()
    assert out.read_text(encoding="utf-8") == f"for {DEVICE}"
    assert "Saved the report to" in capsys.readouterr().out

    thread = _gateway_answers(tmp_path, lambda req: {"ok": False, "error": "That gadget isn't connected right now."})
    with pytest.raises(SystemExit, match="isn't connected"):
        plugin_cli.handle(argparse.Namespace(gadget_command="diag", device=DEVICE, out=str(out)))
    thread.join()
