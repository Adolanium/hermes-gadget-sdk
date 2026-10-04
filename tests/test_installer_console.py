"""The browser installer's console client (site/src/lib/console.js) against the real firmware core,
with a model of ESP-IDF's console between them (tests/fakes/esp_console.py)."""

from __future__ import annotations

import json
import shutil
import subprocess
import threading

import pytest

from conftest import REPO, requires_sim
from fakes.esp_console import respond

pytestmark = [requires_sim, pytest.mark.skipif(shutil.which("node") is None, reason="needs Node.js")]

# Values that only survive the console when quoted and escaped.
SETTINGS = {
    "wifi_ssid": 'Home  "5G"',
    "wifi_pass": 'p\\a "s"  @ok',
    "server": "ws://192.168.1.20:8765/gadget",
    "name": "Desk Gadget",
}


def _serve(node: subprocess.Popen, sim) -> None:
    """The board's side of the serial line: boot noise, a swallowed first command, then echo and reply."""
    node.stdin.write(b"I (312) hg.main: Hermes Gadget 0.1.0 on sim\nW (318) hg.wifi: no Wi-Fi configured\n")
    node.stdin.flush()
    swallowed, pending = False, b""
    while chunk := node.stdout.read1(256):
        pending += chunk
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            if not swallowed:  # ESP-IDF eats input while it probes the terminal at startup
                swallowed = True
                continue
            node.stdin.write(respond(raw.decode("latin-1"), sim.console).encode("latin-1"))
            node.stdin.flush()


def test_installer_settings_reach_the_core_intact(make_sim, tmp_path):
    sim = make_sim("")
    result = tmp_path / "result.json"
    node = subprocess.Popen(["node", str(REPO / "site" / "test" / "console-cli.mjs"), json.dumps(SETTINGS), str(result)],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    board = threading.Thread(target=_serve, args=(node, sim), daemon=True)
    board.start()
    try:
        assert node.wait(timeout=30) == 0, json.loads(result.read_text(encoding="utf-8")).get("error")
    finally:
        node.kill()
    board.join(timeout=5)

    report = json.loads(result.read_text(encoding="utf-8"))
    assert report["before"]["device_id"].startswith("hg-")
    assert (report["after"]["server"], report["after"]["name"]) == (SETTINGS["server"], SETTINGS["name"])
    for key in ("wifi_ssid", "server", "name"):
        assert json.loads(sim.console(f"get {key}")[len("@value "):])["value"] == SETTINGS[key]
    assert sim.storage.get("wifi_pass") == SETTINGS["wifi_pass"]  # `get` only says "<set>" for secrets
    assert any(line.startswith("I (312) hg.main") for line in report["lines"])  # boot noise passed through
