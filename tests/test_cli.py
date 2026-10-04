"""hermes-gadget serial commands, against a fake console."""

from __future__ import annotations

import argparse
import json

import pytest

from fakes.esp_console import respond
from hermes_gadget import cli


class FakeConsole:
    """Answers console lines like the firmware's REPL: echo, then the reply, a few bytes per read."""

    def __init__(self, replies: dict[str, str], chunk: int = 97):
        self.replies = replies
        self.chunk = chunk
        self.pending = b""

    def write(self, data: bytes) -> None:
        line = data.decode().strip()
        self.pending += f"gadget> {line}\n{self.replies[line]}\n".encode()

    def read(self, n: int) -> bytes:
        size = min(n, self.chunk)
        out, self.pending = self.pending[:size], self.pending[size:]
        return out

    def reset_input_buffer(self) -> None:
        self.pending = b""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


REPORT = {
    "device_id": "hg-0123456789abcdef", "board": "esp32s3-touch-amoled-1.75", "firmware": "0.1.0",
    "reset_reason": "brownout", "uptime_s": 42,
    "parts": {"display": "co5300", "microphone": "es7210", "speaker": "es8311", "touch": True, "key": False},
    "i2c": ["0x18", "0x20", "0x34", "0x40", "0x5a"],
    "wifi": {"joined": True, "ssid": "Home", "rssi": -61, "ip": "192.168.1.42"},
    "connection": {"attempts": 3, "last_close": "connection error"},
    "app": {"phase": "connecting", "paired": False, "server": "ws://192.168.1.20:8765/gadget"},
    "stacks": [{"name": f"task{i}", "stack_free": 1000 + i} for i in range(20)],  # a reply many reads long
}


@pytest.fixture
def console(monkeypatch):
    def install(replies):
        fake = FakeConsole(replies)
        monkeypatch.setattr(cli, "_serial", lambda port, baud: fake)
        monkeypatch.setattr(cli.time, "sleep", lambda s: None)
        return fake

    return install


def test_serial_command_waits_for_the_whole_line():
    fake = FakeConsole({"status": "@status " + json.dumps(REPORT)}, chunk=13)
    assert json.loads(cli._serial_command(fake, "status")[len("@status "):]) == REPORT


def test_diag_saves_a_report_and_prints_a_summary(console, tmp_path, capsys):
    console({"diag": "@diag " + json.dumps(REPORT),
             "diag log": "I (310) hg.main: Hermes Gadget 0.1.0\nW (950) hg.codec: speaker missing\n@log end"})
    out_file = tmp_path / "report.txt"
    assert cli.main(["diag", "--port", "COM9", "--out", str(out_file)]) == 0

    text = out_file.read_text(encoding="utf-8")
    assert '"reset_reason": "brownout"' in text
    assert text.rstrip().endswith("I (310) hg.main: Hermes Gadget 0.1.0\nW (950) hg.codec: speaker missing")
    shown = capsys.readouterr().out
    assert "firmware 0.1.0 on esp32s3-touch-amoled-1.75 (hg-0123456789abcdef)" in shown
    assert "reset reason: brownout, up 42 s" in shown
    assert "parts: display co5300, microphone es7210, speaker es8311, touch yes, key no" in shown
    assert "I2C: 0x18 0x20 0x34 0x40 0x5a" in shown
    assert "Wi-Fi: joined Home (-61 dBm), 192.168.1.42" in shown
    assert "Hermes: connecting, not paired, last connection ended: connection error" in shown


def test_diag_explains_a_console_without_it(console, capsys):
    console({"diag": "@error unknown command (try: help)"})  # firmware older than diag
    assert cli.main(["diag", "--port", "COM9"]) == 1
    assert "No diag report from COM9 (@error unknown command" in capsys.readouterr().err


class EspBoard(FakeConsole):
    """A board's settings behind ESP-IDF's console, stored the way App::console parses `set`."""

    def __init__(self):
        super().__init__({})
        self.settings: dict[str, str] = {}

    def app(self, line: str) -> str:
        command, _, rest = line.partition(" ")
        if command == "set":
            key, _, value = rest.partition(" ")
            self.settings[key] = value.strip()
            return f"@ok {key}"
        return "@status {}" if command == "status" else "@error unknown command"

    def write(self, data: bytes) -> None:
        self.pending += respond(data.decode(), self.app).encode()


def _provision(monkeypatch, board, **values):
    monkeypatch.setattr(cli, "_serial", lambda port, baud: board)
    monkeypatch.setattr(cli.time, "sleep", lambda s: None)
    settings = {"wifi_ssid": None, "wifi_pass": None, "server": None, "token": None, "name": None, **values}
    return cli.cmd_provision(argparse.Namespace(port="COM5", baud=115200, **settings))


def test_provision_gets_every_character_through_the_console(monkeypatch):
    board = EspBoard()
    values = {"wifi_ssid": 'Home  "5G"', "wifi_pass": 'p\\a "s"  @ok', "server": "ws://192.168.1.20:8765/gadget",
              "name": "Desk Gadget"}
    assert _provision(monkeypatch, board, **values) == 0
    assert board.settings == values


def test_provision_refuses_what_the_console_would_drop(monkeypatch, capsys):
    board = EspBoard()
    assert _provision(monkeypatch, board, wifi_ssid="Café", wifi_pass="secret") == 2
    assert board.settings == {}
    assert "wifi_ssid: the board's console only takes printable ASCII" in capsys.readouterr().err
