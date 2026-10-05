"""Run the examples against local HTTP/MQTT peers and the production gadget core."""

import asyncio
import importlib.util
import json
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

import pytest
from conftest import REPO, requires_sim
from hermes_gadget.linux.client import Client
from hermes_gadget_plugin.hub import ActionError


def load_example(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "examples/automation" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bridge = load_example("bridge")
ha = load_example("home_assistant")


@pytest.fixture
def home_assistant():
    state = SimpleNamespace(calls=[], status=200, value="23.5", unit="°C", gate=None)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.respond({"state": state.value, "attributes": {"unit_of_measurement": state.unit}})

        def do_POST(self):
            if state.gate:
                state.gate.wait(5)
            self.respond([])

        def respond(self, response):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            state.calls.append((self.command, self.path, self.headers.get("Authorization"),
                                json.loads(body) if body else None))
            self.send_response(state.status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Location", "http://127.0.0.1:1/do-not-follow")
            self.end_headers()
            self.wfile.write(json.dumps(response).encode())

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    backend = ha.HomeAssistant(f"http://127.0.0.1:{server.server_port}", "test-token", "light.desk", "sensor.room")
    try:
        yield backend, state
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_home_assistant_uses_services_and_rejects_unknown_readings_and_redirects(home_assistant):
    backend, state = home_assistant
    assert backend.read_temperature() == 23.5
    assert backend.set_lamp(False) == {"service_executed": True, "requested_on": False}
    assert state.calls == [
        ("GET", "/api/states/sensor.room", "Bearer test-token", None),
        ("POST", "/api/services/light/turn_off", "Bearer test-token", {"entity_id": "light.desk"}),
    ]
    for value in ("unavailable", "unknown", "NaN", "Infinity", "900", True, None):
        state.value = value
        assert backend.read_temperature() is None
    state.value, state.unit = "72", "°F"
    assert backend.read_temperature() is None
    state.status = 302
    with pytest.raises(RuntimeError, match="Home Assistant request failed"):
        backend.set_lamp(True)
    assert state.calls[-1][1] == "/api/services/light/turn_on"


@pytest.mark.parametrize("lamp,sensor", [("lock.door", "sensor.room"),
                                          ("light.desk/../../lock", "sensor.room"),
                                          ("light.desk", "sensor.room?token=x")])
def test_home_assistant_entities_are_fixed_at_startup(lamp, sensor):
    with pytest.raises(ValueError):
        ha.HomeAssistant("https://home.example", "token", lamp, sensor)


@requires_sim
def test_bridge_sensor_action_and_failure_use_the_core(devserver, loop_thread, tmp_path, home_assistant):
    backend, state = home_assistant
    hub, brain, url = devserver()
    events = []

    async def on_event(session, name, data, notify):
        events.append((name, data, notify))

    brain.on_event = on_event
    client = Client({"server": url}, tmp_path)
    example = bridge.Bridge(client.device, backend)

    def wait(predicate):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            client.step()
            example.step()
            if predicate():
                return
            time.sleep(0.01)
        pytest.fail("example did not complete")

    def invoke(name, args):
        future = asyncio.run_coroutine_threadsafe(session.invoke_action(name, args), loop_thread.loop)
        wait(future.done)
        return future.result()

    try:
        client.start()
        wait(lambda: client.device.status()["paired"])
        session = next(iter(hub.sessions.values()))
        assert session.action_names() == ["lamp.set", "automation.status"]
        wait(lambda: session.sensors.get("temperature_c") == 23.5)
        assert session.sensors["temperature_available"] == 1
        state.gate = threading.Event()
        accepted = invoke("lamp.set", {"on": True})
        assert accepted["accepted"] is True
        assert invoke("automation.status", {"job": accepted["job"]}) == {
            "job": accepted["job"], "state": "pending"}
        with pytest.raises(ActionError, match="still pending"):
            invoke("lamp.set", {"on": False})
        assert client.device.status()["phase"] == "online"
        state.gate.set()
        wait(lambda: bool(events))
        result = invoke("automation.status", {"job": accepted["job"]})
        assert result == {"job": accepted["job"], "state": "completed",
                          "result": {"service_executed": True, "requested_on": True}}
        assert events == [("automation.completed", result, False)]
        assert state.calls[-1] == ("POST", "/api/services/light/turn_on", "Bearer test-token", {"entity_id": "light.desk"})
        before = len(state.calls)
        with pytest.raises(ActionError, match="expected only on"):
            invoke("lamp.set", {"on": True, "entity_id": "light.other"})
        assert len(state.calls) == before
        state.status = 500
        accepted = invoke("lamp.set", {"on": False})
        wait(lambda: len(events) == 2)
        result = invoke("automation.status", {"job": accepted["job"]})
        assert result == {"job": accepted["job"], "state": "failed",
                          "error": "request failed or timed out; outcome may be unknown"}
        example.next_read = 0
        wait(lambda: session.sensors.get("temperature_available") == 0)
    finally:
        if state.gate:
            state.gate.set()
        example.close()
        client.close()


@pytest.fixture
def mqtt_peer():
    packets = []
    state = SimpleNamespace(acknowledge=True)

    class Handler(socketserver.BaseRequestHandler):
        def read(self, size):
            result = b""
            while len(result) < size:
                data = self.request.recv(size - len(result))
                if not data:
                    raise EOFError
                result += data
            return result

        def handle(self):
            self.request.settimeout(5)
            try:
                while True:
                    header = self.read(1)[0]
                    length, multiplier = 0, 1
                    while True:
                        byte = self.read(1)[0]
                        length += (byte & 127) * multiplier
                        if not byte & 128:
                            break
                        multiplier *= 128
                    data = self.read(length)
                    if header >> 4 == 1:
                        self.request.sendall(b"\x20\x02\x00\x00")  # CONNACK
                    elif header >> 4 == 8:
                        packets.append(("subscribe", data[4:-1].decode()))
                        self.request.sendall(b"\x90\x03" + data[:2] + b"\x00")
                        body = b"\x00\x09room/temp21.75"
                        self.request.sendall(bytes([0x30, len(body)]) + body)
                    elif header >> 4 == 3:
                        size = int.from_bytes(data[:2], "big")
                        topic = data[2:2 + size].decode()
                        mid, payload = data[2 + size:4 + size], data[4 + size:]
                        packets.append(("publish", header, topic, payload))
                        if state.acknowledge:
                            self.request.sendall(b"\x40\x02" + mid)  # PUBACK
                    elif header >> 4 == 14:
                        return
            except (EOFError, OSError):
                pass

    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1], packets, state
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_mqtt_real_client_topics_acknowledgement_and_stale_readings(mqtt_peer):
    pytest.importorskip("paho.mqtt.client")
    module = load_example("mqtt")
    port, packets, _ = mqtt_peer
    backend = module.Mqtt("127.0.0.1", port, "desk/set", "room/temp", tls=False)
    try:
        deadline = time.monotonic() + 5
        while backend.read_temperature() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        assert backend.read_temperature() == 21.75
        assert backend.set_lamp(True) == {"broker_acknowledged": True, "requested_on": True}
        assert packets == [("subscribe", "room/temp"), ("publish", 0x32, "desk/set", b"ON")]
        for payload, retained in [(b"NaN", False), (b"22", True), (b"5" * 129, False)]:
            backend.message(None, None, SimpleNamespace(topic="room/temp", payload=payload, retain=retained))
            assert backend.read_temperature() is None
        backend.sample = (22, time.monotonic() - 61)
        assert backend.read_temperature() is None
        backend.disconnected(None, None, None, None, None)
        assert backend.sample == (None, 0.0)
    finally:
        backend.close()


def test_mqtt_missing_ack_is_a_failure_and_does_not_reconnect_the_command(mqtt_peer):
    pytest.importorskip("paho.mqtt.client")
    module = load_example("mqtt")
    port, packets, state = mqtt_peer
    state.acknowledge = False
    backend = module.Mqtt("127.0.0.1", port, "desk/set", "room/temp", tls=False)
    try:
        with pytest.raises(RuntimeError, match="acknowledgement timed out"):
            backend.set_lamp(False)
        assert [p for p in packets if p[0] == "publish"] == [("publish", 0x32, "desk/set", b"OFF")]
    finally:
        backend.close()


def test_mqtt_rejects_wildcard_and_shared_topics():
    pytest.importorskip("paho.mqtt.client")
    module = load_example("mqtt")
    for lamp, sensor in [("desk/#", "room/temp"), ("desk/set", "+/temp"), ("desk/set", "desk/set")]:
        with pytest.raises(ValueError):
            module.Mqtt("localhost", 1883, lamp, sensor)
