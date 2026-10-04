"""`hermes gateway setup` → Hermes Gadget, and `hermes gadget pair`, with Hermes's side faked."""

from __future__ import annotations

import types
from urllib.parse import unquote

import pytest

from hermes_gadget_plugin import cli, setup

DESK, KITCHEN = "hg-0123456789abcdef", "hg-fedcba9876543210"
SERVER = "ws://192.168.1.20:8765/gadget"


def test_the_installer_link_carries_the_server_in_the_fragment():
    link = cli.installer_link(SERVER)
    base, _, fragment = link.partition("#")
    assert base == cli.INSTALLER_URL and "?" not in base  # nothing for the web host to see
    assert fragment.startswith("server=") and unquote(fragment[len("server="):]) == SERVER


def _setup(monkeypatch, answer: str, extra: dict, *, writable: bool = True):
    writes, shown = [], []

    def prompt(question, default=None, password=False):
        shown.append(f"{question} [{default}]")
        return answer or default

    monkeypatch.setattr(setup, "_ui", lambda: (shown.append, shown.append, shown.append, shown.append, prompt))
    monkeypatch.setattr(setup, "_set_config", lambda key, value: writes.append((key, value)) or writable)
    monkeypatch.setattr(cli, "_gadget_extra", lambda: dict(extra))
    monkeypatch.setattr(cli, "_lan_address", lambda: "192.168.1.20")
    setup.interactive_setup()
    return writes, shown


def test_setup_enables_the_platform_and_points_at_the_installer(monkeypatch):
    writes, shown = _setup(monkeypatch, "", {})
    assert writes == [("platforms.gadget.enabled", "true")]
    assert "Port devices connect to [8765]" in shown
    assert f"  {cli.installer_link(SERVER)}" in shown
    assert any("hermes gadget pair" in line for line in shown)


def test_setup_writes_a_changed_port_and_keeps_the_old_one_for_nonsense(monkeypatch):
    writes, shown = _setup(monkeypatch, "9000", {})
    assert writes == [("platforms.gadget.enabled", "true"), ("platforms.gadget.extra.port", "9000")]
    assert "Devices connect to ws://192.168.1.20:9000/gadget" in shown

    writes, shown = _setup(monkeypatch, "http", {"port": 8800})
    assert writes == [("platforms.gadget.enabled", "true")]
    assert "'http' isn't a port number; keeping 8800" in shown
    assert "Devices connect to ws://192.168.1.20:8800/gadget" in shown


def test_setup_says_what_to_run_when_it_cannot_write_the_config(monkeypatch):
    _, shown = _setup(monkeypatch, "", {}, writable=False)
    assert any("hermes config set platforms.gadget.enabled true" in line for line in shown)


class FakeDevices:
    """The plugin's device registry as the gateway leaves it: enrolled devices and the codes they show."""

    def __init__(self, devices: dict, codes: dict):
        self._devices, self._codes = devices, codes

    def devices(self):
        return {k: dict(v) for k, v in self._devices.items()}

    def pairing_for(self, device_id):
        code = self._codes.get(device_id)
        return (code, f"hermes pairing approve gadget {code}") if code else None


class FakeApprovals:
    """Hermes's PairingStore: pending codes and the approved list."""

    def __init__(self, pending: dict, approved=()):
        self.pending, self.approved = dict(pending), set(approved)

    def is_approved(self, platform, user_id):
        return platform == "gadget" and user_id in self.approved

    def approve_code(self, platform, code):
        device_id = self.pending.pop(code, None) if platform == "gadget" else None
        if device_id is None:
            return None
        self.approved.add(device_id)
        return {"user_id": device_id, "user_name": ""}


def _pair(monkeypatch, stores, approvals, *, yes=False, answers=(), timeout=0.0):
    stores = iter(stores) if isinstance(stores, list) else iter([stores] * 1000)
    monkeypatch.setattr(cli, "_store", lambda: next(stores))
    monkeypatch.setattr(cli, "_pairing_store", lambda: approvals)
    monkeypatch.setattr(cli.time, "sleep", lambda seconds: None)
    replies = iter(answers)
    monkeypatch.setattr("builtins.input", lambda question: next(replies))
    cli._cmd_pair(types.SimpleNamespace(yes=yes, timeout=timeout))


def test_pair_approves_the_device_showing_a_code(monkeypatch, capsys):
    devices = FakeDevices({DESK: {"name": "Desk", "board": "esp32s3-touch-amoled-1.75"}}, {DESK: "ABCD2345"})
    approvals = FakeApprovals({"ABCD2345": DESK})
    _pair(monkeypatch, devices, approvals, answers=[""])
    assert approvals.approved == {DESK}
    assert "Approved Desk. It shows Ready" in capsys.readouterr().out


def test_pair_leaves_declined_and_already_approved_devices_alone(monkeypatch, capsys):
    devices = FakeDevices({DESK: {"name": "Desk"}, KITCHEN: {"name": "Kitchen"}},
                          {DESK: "ABCD2345", KITCHEN: "EFGH6789"})
    approvals = FakeApprovals({"EFGH6789": KITCHEN}, approved={DESK})
    with pytest.raises(SystemExit) as stop:
        _pair(monkeypatch, devices, approvals, answers=["n"])
    assert stop.value.code == 1
    assert approvals.approved == {DESK}
    assert "Left Kitchen unapproved." in capsys.readouterr().out


def test_pair_says_when_hermes_no_longer_knows_the_code(monkeypatch, capsys):
    devices = FakeDevices({DESK: {"name": "Desk"}}, {DESK: "ABCD2345"})
    with pytest.raises(SystemExit):
        _pair(monkeypatch, devices, FakeApprovals({}), yes=True)
    assert "it may have expired" in capsys.readouterr().out


def test_pair_waits_for_a_device_to_ask(monkeypatch, capsys):
    nobody, desk = FakeDevices({}, {}), FakeDevices({DESK: {"name": "Desk"}}, {DESK: "ABCD2345"})
    approvals = FakeApprovals({"ABCD2345": DESK})
    _pair(monkeypatch, [nobody, nobody, desk], approvals, yes=True, timeout=60)
    out = capsys.readouterr().out
    assert out.count("Waiting for a gadget") == 1 and "Approved Desk" in out

    with pytest.raises(SystemExit) as stop:
        _pair(monkeypatch, nobody, approvals, yes=True, timeout=0)
    assert "No gadget asked to pair" in str(stop.value)
