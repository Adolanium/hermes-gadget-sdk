"""Linux device behavior against the real native core, hub and local socket."""

import json
import os
import socket
import subprocess
import sys
import time

import pytest
from conftest import REPO, requires_sim
from hermes_gadget.linux.client import Client, State, load_config


def wait(client, predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        client.step()
        if predicate():
            return
        time.sleep(0.01)
    pytest.fail(f"device did not reach expected state: {client.command({'command': 'status'})}")


@requires_sim
def test_headless_pairs_sends_text_and_keeps_identity(devserver, loop_thread, tmp_path):
    hub, brain, url = devserver(require_pairing=True)
    config = {"server": url, "name": "Kitchen"}
    client = Client(config, tmp_path / "device")
    try:
        client.start()
        wait(client, lambda: client.device.status().get("pairing_code"))
        assert client.command({"command": "status"})["board"] == "linux"
        with pytest.raises(ValueError, match="not paired"):
            client.command({"command": "send", "text": "too early"})
        code = client.command({"command": "status"})["pairing_code"]
        assert loop_thread.run(brain.approve(code)) is True
        wait(client, lambda: client.device.status()["paired"])
        session = next(iter(hub.sessions.values()))
        assert session.caps == {"inputs": ["talk", "cancel"], "talk_mode": "hold"}
        assert session.sensors == {}
        assert session.action_names() == []
        client.command({"command": "send", "text": "Hello kitchen"})
        wait(client, lambda: any(m.get("type") == "reply" for m in client.messages))
        reply = next(m for m in client.command({"command": "messages"})["messages"] if m["type"] == "reply")
        assert reply["text"] == "You said: Hello kitchen"
        identity = client.device.status()["device_id"]
        key = hub.store.key_for(identity)
    finally:
        client.close()
    restarted = Client(config, tmp_path / "device")
    try:
        restarted.start()
        wait(restarted, lambda: restarted.device.status()["paired"])
        assert restarted.device.status()["device_id"] == identity
        assert hub.store.key_for(identity) == key
        assert restarted.command({"command": "status"})["phase"] == "online"
    finally:
        restarted.close()


@requires_sim
def test_linux_reconnects_and_reports_events(devserver, loop_thread, tmp_path):
    hub, brain, url = devserver()
    events = []

    async def on_event(session, name, data, notify):
        events.append((name, data, notify))

    brain.on_event = on_event
    client = Client({"server": url}, tmp_path / "device")
    try:
        client.start()
        wait(client, lambda: client.device.status()["paired"])
        original = next(iter(hub.sessions.values()))
        loop_thread.run(original.close("reconnect test"))
        wait(client, lambda: bool(hub.sessions) and next(iter(hub.sessions.values())) is not original
             and client.device.status()["paired"])
        client.command({"command": "event", "name": "door.opened", "data": {"room": "kitchen"}, "notify": True})
        wait(client, lambda: bool(events))
        assert events == [("door.opened", {"room": "kitchen"}, True)]
        with pytest.raises(ValueError, match="notify"):
            client.command({"command": "event", "name": "door.opened", "notify": "false"})
        with pytest.raises(ValueError, match="unknown command"):
            client.command({"command": "shell", "text": "id"})
    finally:
        client.close()


def test_config_and_corrupt_identity_fail_without_replacing_state(tmp_path):
    config = tmp_path / "config.json"
    config.write_text('{"server":"wss://host.example/gadget","name":"Kitchen"}')
    assert load_config(config) == {"server": "wss://host.example/gadget", "name": "Kitchen"}
    for text in ['[]', '{"server":"http://host"}', '{"server":"ws://a:b@host"}',
                 '{"server":"ws://host","shell":"id"}']:
        config.write_text(text)
        with pytest.raises(ValueError):
            load_config(config)
    state = tmp_path / "device.json"
    state.write_text('{"device_key":"bad"}')
    with pytest.raises(ValueError):
        State(tmp_path)
    assert state.read_text() == '{"device_key":"bad"}'
    state.write_text('{"device_key":')
    with pytest.raises(ValueError):
        State(tmp_path)
    assert state.read_text() == '{"device_key":'


@requires_sim
def test_identity_write_failure_stops_before_enrollment(devserver, tmp_path, monkeypatch):
    hub, _, url = devserver()
    client = Client({"server": url}, tmp_path / "device")

    def full_disk():
        raise OSError("disk full")

    monkeypatch.setattr(client.state, "save", full_disk)
    try:
        with pytest.raises(RuntimeError, match="could not be saved"):
            client.start()
        assert hub.store.devices() == {}
    finally:
        client.close()


@requires_sim
@pytest.mark.skipif(sys.platform != "linux", reason="Linux process and Unix socket")
def test_service_socket_is_private_exclusive_and_stops_on_sigterm(tmp_path):
    from hermes_gadget.linux.control import device_lock, request

    directory = tmp_path / "device"
    config = tmp_path / "config.json"
    config.write_text('{"server":"ws://127.0.0.1:1/gadget","name":"Offline"}')
    env = {**os.environ, "PYTHONPATH": str(REPO / "python")}
    process = subprocess.Popen([sys.executable, "-m", "hermes_gadget", "linux", "--state-dir", str(directory),
                                "run", "--config", str(config)], env=env)
    try:
        deadline = time.monotonic() + 10
        while not (directory / "control.sock").exists() and time.monotonic() < deadline:
            assert process.poll() is None
            time.sleep(0.02)
        assert request(directory, {"command": "status"})["name"] == "Offline"
        assert directory.stat().st_mode & 0o777 == 0o700
        assert (directory / "device.json").stat().st_mode & 0o777 == 0o600
        assert (directory / "control.sock").stat().st_mode & 0o777 == 0o600
        with pytest.raises(RuntimeError, match="another gadget"), device_lock(directory):
            pytest.fail("second service acquired the device lock")
        # An unfinished request cannot block the device loop or other clients.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
            slow.connect(str(directory / "control.sock"))
            slow.sendall(b'{"command":')
            assert request(directory, {"command": "status"})["board"] == "linux"
        assert request(directory, {"command": "send", "text": "offline"}) == {
            "error": "device is not paired and online; check status"}
    finally:
        process.terminate()
        process.wait(timeout=10)
    assert process.returncode == 0
    assert not (directory / "control.sock").exists()
    assert len(json.loads((directory / "device.json").read_text())["device_key"]) == 44
