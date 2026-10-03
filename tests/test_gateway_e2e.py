"""Full end-to-end: a real ``hermes gateway run`` with the gadget plugin
installed, a simulated device, and a fake OpenAI-compatible server standing in
for the model, speech-to-text and text-to-speech.

Opt-in (spawns processes, ~1 minute):
    HERMES_GADGET_E2E=1 pytest tests/test_gateway_e2e.py
Needs a Hermes Agent checkout with a working virtualenv (HERMES_AGENT_DIR,
default ../hermes-agent; the gateway runs with <dir>/.venv's Python).
"""

from __future__ import annotations

import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from conftest import REPO, requires_sim

AGENT_DIR = Path(os.environ.get("HERMES_AGENT_DIR", REPO.parent / "hermes-agent"))
VENV_PY = AGENT_DIR / ".venv" / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")

pytestmark = [
    requires_sim,
    pytest.mark.skipif(os.environ.get("HERMES_GADGET_E2E") != "1", reason="set HERMES_GADGET_E2E=1 to run"),
    pytest.mark.skipif(not VENV_PY.exists(), reason=f"no Hermes virtualenv at {VENV_PY}"),
]

CONFIG = """\
model:
  provider: custom
  default: fake-gadget-model
  base_url: http://127.0.0.1:{api}/v1
  api_key: test-key
platforms:
  gadget:
    enabled: true
    extra:
      host: 127.0.0.1
      port: {gadget}
      unauthorized_dm_behavior: pair
plugins:
  enabled: [gadget]
streaming:
  enabled: true   # stream reply text to messaging platforms (the device) as it is generated
stt:
  enabled: true
  provider: openai
  openai: {{api_key: test-key, base_url: "http://127.0.0.1:{api}/v1", model: whisper-1}}
tts:
  provider: openai
  openai: {{api_key: test-key, base_url: "http://127.0.0.1:{api}/v1", model: tts-1, voice: alloy}}
"""


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _hermes(home: Path, *args: str, **kw) -> subprocess.CompletedProcess:
    env = {**os.environ, "HERMES_HOME": str(home), "PYTHONUTF8": "1"}
    return subprocess.run([str(VENV_PY), "-m", "hermes_cli.main", *args], cwd=AGENT_DIR, env=env,
                          capture_output=True, text=True, timeout=180, **kw)


@pytest.fixture
def gateway(tmp_path):
    from fakes import fake_openai

    api_server, api_port = fake_openai.start(0)
    gadget_port = _free_port()
    home = tmp_path / "hermes-home"
    (home / "plugins").mkdir(parents=True)
    shutil.copytree(REPO / "plugin", home / "plugins" / "gadget", ignore=shutil.ignore_patterns("__pycache__"))
    (home / "config.yaml").write_text(CONFIG.format(api=api_port, gadget=gadget_port), encoding="utf-8")
    env = {**os.environ, "HERMES_HOME": str(home), "PYTHONUTF8": "1"}
    log = open(tmp_path / "gateway.out", "w", encoding="utf-8")
    proc = subprocess.Popen([str(VENV_PY), "-m", "hermes_cli.main", "gateway", "run"], cwd=AGENT_DIR, env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 120
    url = f"ws://127.0.0.1:{gadget_port}/gadget"
    while time.monotonic() < deadline:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", gadget_port)) == 0:
                break
        if proc.poll() is not None:
            pytest.fail(f"gateway exited: {(tmp_path / 'gateway.out').read_text(errors='replace')[-3000:]}")
        time.sleep(0.5)
    else:
        pytest.fail("gateway did not open the gadget port")
    yield {"url": url, "home": home, "api": fake_openai.Handler.requests_log}
    proc.terminate()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    api_server.shutdown()


def test_device_pairs_talks_and_is_driven_by_the_agent(gateway, make_sim):
    sim = make_sim(gateway["url"], name="E2E Gadget")

    # Pairing goes through Hermes's own DM pairing codes.
    assert sim.wait_for(lambda: sim.status().get("pairing_code"), timeout=60)
    code = sim.status()["pairing_code"]
    approved = _hermes(gateway["home"], "pairing", "approve", "gadget", code)
    assert approved.returncode == 0, approved.stdout + approved.stderr
    assert sim.wait_screen("ready", timeout=30)

    # Text in, model reply out, streamed as it is generated.
    sim.received.clear()
    sim.type_text("hello hermes")
    assert sim.wait_for(lambda: "You said: hello hermes" in (sim.last_received("reply") or {}).get("text", ""),
                        timeout=90)
    assert sim.last_received("reply.delta") is not None, [m["type"] for m in sim.received]
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=30)

    # Voice in: Hermes STT -> model -> reply spoken through Hermes TTS.
    sim.received.clear()
    pcm = b"".join(struct.pack("<h", int(7000 * math.sin(2 * math.pi * 300 * i / 16000))) for i in range(16000))
    sim.speak_pcm(pcm)
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=90)
    assert sim.last_received("transcript")["text"] == "what is on my screen"
    assert "what is on my screen" in sim.last_received("reply")["text"]
    assert sim.last_received("audio.start") is not None

    # The agent drives the device through the gadget tools.
    sim.received.clear()
    sim.type_text("put a note on my screen")
    assert sim.wait_for(lambda: sim.last_received("display") is not None, timeout=90)
    assert sim.last_received("display")["body"] == "Hello from Hermes"
    sim.tap("cancel")
    sim.received.clear()
    sim.type_text("turn the led on")
    assert sim.wait_for(lambda: sim.peripherals.led == "green", timeout=90)
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=60)

    # Holding CANCEL starts a new Hermes session; the hold was the confirmation, so the
    # gateway's "Confirm /new" is answered by the plugin and the reset reply arrives.
    sim.received.clear()
    sim.console("new-session")
    assert sim.wait_for(lambda: sim.last_received("reply") is not None, timeout=60), sim.received
    assert "Confirm" not in sim.last_received("reply")["text"]
    assert sim.last_received("prompt") is None

    # A typed /new asks on the device instead, and TALK answers yes.
    sim.run_for(1.0)
    sim.received.clear()
    sim.type_text("/new")
    assert sim.wait_screen("prompt", timeout=60), sim.received
    assert "/new" in sim.last_received("prompt")["title"]
    sim.run_for(0.7)
    sim.tap("talk")
    assert sim.wait_for(lambda: sim.last_received("reply") is not None, timeout=60), sim.received
    assert sim.device.screen() != "prompt"
    paths = [r["path"] for r in gateway["api"]]
    assert any(p.endswith("/audio/transcriptions") for p in paths)
    assert any(p.endswith("/audio/speech") for p in paths)
