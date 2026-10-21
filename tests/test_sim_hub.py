"""The production device core (in the simulator) against the production hub."""

import json
import math
import struct

import pytest

from conftest import requires_sim
from hermes_gadget_plugin.hub import HubDelegate

pytestmark = requires_sim


def _tone(seconds: float, rate: int = 16000) -> bytes:
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 330 * i / rate)))
                    for i in range(int(rate * seconds)))


def test_open_device_reaches_ready_and_text_round_trips(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    sim.type_text("ping")
    assert sim.wait_for(lambda: (sim.last_received("turn.end") or {}).get("outcome") == "success", timeout=10)
    assert sim.last_received("reply")["text"] == "You said: ping"
    assert sim.wait_screen("ready", timeout=5)


def test_pairing_code_flow(loop_thread, devserver, make_sim):
    hub, brain, url = devserver(require_pairing=True)
    sim = make_sim(url)
    assert sim.wait_screen("pairing", timeout=10)
    assert sim.wait_for(lambda: sim.status().get("pairing_code"), timeout=5)
    code = sim.status()["pairing_code"]
    sim.press("talk")  # refused while unpaired
    sim.run_for(0.2)
    assert sim.device.screen() == "pairing"
    assert loop_thread.run(brain.approve(code))
    assert sim.wait_screen("ready", timeout=5)


def test_device_key_is_enrolled_once_then_proven(devserver, make_sim, tmp_path):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    device_id = sim.status()["device_id"]
    assert hub.store.key_for(device_id) is not None
    # The registry keeps what the device reported, for `hermes gadget devices` and `update --latest`.
    record = hub.store.devices()[device_id]
    assert (record["board"], record["firmware"]) == (sim.status()["board"], sim.status()["firmware"])
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_screen("ready", timeout=10)
    challenge = [m for m in sim.received if m.get("type") == "challenge"]
    assert challenge[0]["enrolled"] is False and challenge[-1]["enrolled"] is True


def _raw_handshake(url: str, device_id: str, auth: dict) -> dict:
    """Speak the handshake by hand, as a hostile client would."""
    from websockets.sync.client import connect

    with connect(url, subprotocols=["hermes-gadget.v1"]) as ws:
        ws.send(json.dumps({"type": "hello", "proto": 1, "device_id": device_id, "name": "x"}))
        challenge = json.loads(ws.recv(timeout=5))
        ws.send(json.dumps({"type": "auth", **auth(challenge)}))
        return json.loads(ws.recv(timeout=5))


def test_impostors_cannot_take_over_an_enrolled_device(devserver, make_sim):
    import base64

    from hermes_gadget_plugin import protocol

    hub, _brain, url = devserver()
    real = make_sim(url)
    assert real.wait_screen("ready", timeout=10)
    victim = real.status()["device_id"]
    other_key = bytes(range(1, 33))
    # Wrong key, valid-looking MAC.
    reply = _raw_handshake(url, victim, lambda ch: {"mac": protocol.auth_mac(other_key, victim, ch["nonce"])})
    assert reply == {"type": "error", "code": "auth_failed", "message": reply["message"]}
    # Replaying an enrollment for someone else's id.
    reply = _raw_handshake(url, "hg-0000000000000000", lambda ch: {"key": base64.b64encode(other_key).decode()})
    assert reply["code"] == "auth_failed"
    # The genuine device is unaffected.
    real.console("reconnect")
    assert real.wait_screen("ready", timeout=10)


def test_wrong_access_token_is_refused(devserver, make_sim):
    hub, _brain, url = devserver(token="sekrit")
    bad = make_sim(url, state="bad", token="nope")
    assert bad.wait_for(lambda: (bad.last_received("error") or {}).get("code") == "bad_token", timeout=10)
    assert bad.wait_screen("error", timeout=5)
    good = make_sim(url, state="good", token="sekrit")
    assert good.wait_screen("ready", timeout=10)


def test_voice_is_uploaded_as_wav_and_played_back(devserver, make_sim):
    hub, brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    seconds = sim.speak_pcm(_tone(0.8))
    assert sim.wait_screen("listening", timeout=2)
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=15)
    reply = sim.last_received("reply")["text"]
    assert "seconds of audio" in reply
    heard = float(reply.split("I heard ")[1].split(" seconds")[0])
    assert abs(heard - seconds - 0.3) < 0.25  # includes the release tail
    assert sim.wait_screen("ready", timeout=10)
    assert sim.speaker.last_file is not None and sim.speaker.last_file.stat().st_size > 20000


def test_cancel_interrupts_the_turn(devserver, make_sim):
    hub, brain, url = devserver()
    brain.word_delay = 0.3
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    sim.type_text("one two three four five six seven eight nine ten")
    assert sim.wait_screen("responding", timeout=5)
    sim.tap("cancel")
    assert sim.wait_for(lambda: (sim.last_received("turn.end") or {}).get("outcome") == "cancelled", timeout=5)
    assert sim.wait_screen("ready", timeout=5)


def test_stopping_the_hub_cancels_work_devices_started(loop_thread, devserver, make_sim):
    """A reply still streaming (or a reminder still waiting) must not outlive the server."""
    hub, brain, url = devserver()
    brain.word_delay = 5.0
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    sim.type_text("one two three")
    assert sim.wait_screen("responding", timeout=5)
    turn = brain._turns[sim.status()["device_id"]]

    loop_thread.run(hub.stop())

    assert turn.cancelled()


def test_agent_invokes_device_actions(loop_thread, devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert {"led.set", "buzzer.beep", "speaker.volume", "screen.brightness"} <= set(session.action_names())

    import asyncio

    # The device answers on its own thread, so step the simulator while the hub waits.
    ok = asyncio.run_coroutine_threadsafe(session.invoke_action("led.set", {"color": "red"}), loop_thread.loop)
    assert sim.wait_for(ok.done, timeout=5)
    assert ok.result() == {"led": "red"}
    assert sim.peripherals.led == "red"

    bad = asyncio.run_coroutine_threadsafe(session.invoke_action("led.set", {}), loop_thread.loop)
    assert sim.wait_for(bad.done, timeout=5)
    assert "color is required" in str(bad.exception())


def test_cards_and_images_take_over_the_screen(loop_thread, devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    loop_thread.run(session.show_card("Timer", "Pasta: 9 minutes", ttl_s=0))
    assert sim.wait_screen("card", timeout=5)
    sim.tap("cancel")
    assert sim.wait_screen("ready", timeout=5)

    box_w, box_h = session.image_box
    w, h = 64, 48
    assert w <= box_w and h <= box_h
    pixels = struct.pack("<H", 0xF800) * (w * h)  # pure red
    loop_thread.run(session.show_image(w, h, pixels, ttl_s=0))
    assert sim.wait_screen("image", timeout=5)
    sim.run_for(0.2)
    rgb = sim.rgb888()
    width = sim.board.width
    cy = sim.board.height // 2
    center = rgb[(cy * width + width // 2) * 3:(cy * width + width // 2) * 3 + 3]
    assert center == b"\xff\x00\x00"


def test_sensor_state_reaches_the_hub(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    sim.set_sensor("temperature_c", 30.5)
    assert sim.wait_for(lambda: next(iter(hub.sessions.values())).sensors.get("temperature_c") == 30.5, timeout=5)


_POWER_FIELDS = ("battery_present", "battery_mv", "battery_percent", "charging", "external_power")


def _wait_power(sim, hub, reading):
    """Observe a normal HAL poll and its raw telemetry delivery, within one deadline."""
    expected = {"available": reading is not None}
    if reading is not None:
        expected.update({key: value for key, value in reading.items() if value is not None})
    sensors = {"power_available": int(reading is not None)}
    for key in _POWER_FIELDS:
        value = expected.get(key)
        sensors[key] = int(value) if type(value) is bool else value

    def delivered():
        session = hub.sessions.get(sim.status()["device_id"])
        return (sim.status().get("power") == expected and session is not None
                and all(session.sensors.get(key) == value for key, value in sensors.items()))

    assert sim.wait_for(delivered, timeout=9.8), (sim.status(), sensors)
    # Status and sensor delivery can precede the next normal 100 ms UI frame.
    sim.run_for(0.12)


def _power_header(sim):
    # The 240x240 profile's first ten rows contain the title, battery and link
    # glyphs, but not the animated ready-screen content below the top bar.
    return sim.rgb888(0, 10)


def _power_sim(make_sim, url, **kwargs):
    sim = make_sim(url, board="sim-240x240", name="Waveshare 1.54", **kwargs)
    assert sim.wait_screen("ready", timeout=10)
    sim.console("set battery_display percentage")
    return sim


def test_voltage_estimate_is_opt_in_display_only_and_clears(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = _power_sim(make_sim, url)
    _wait_power(sim, hub, None)
    hidden = _power_header(sim)
    voltage = {"battery_mv": 3840, "charging": False}
    sim.set_power(voltage)
    _wait_power(sim, hub, voltage)
    assert _power_header(sim) == hidden
    sim.set_sensor("battery_pct", 87)
    sim.run_for(0.1)
    assert _power_header(sim) == hidden

    sim.console("set battery_estimate liion")
    sim.run_for(0.05)
    estimated = _power_header(sim)
    assert estimated != hidden
    assert "battery_percent" not in sim.status()["power"]
    assert hub.sessions[sim.status()["device_id"]].sensors.get("battery_percent") is None

    direct = {**voltage, "battery_percent": 50}
    sim.set_power(direct)
    _wait_power(sim, hub, direct)
    assert _power_header(sim) == estimated
    sim.set_power(voltage)
    _wait_power(sim, hub, voltage)
    sim.console("set battery_estimate off")
    sim.run_for(0.05)
    assert _power_header(sim) == hidden
    sim.console("set battery_estimate liion")
    sim.run_for(0.05)
    assert _power_header(sim) == estimated
    sim.console("set battery_estimate")
    sim.run_for(0.05)
    assert _power_header(sim) == hidden
    sim.console("set battery_estimate liion")
    sim.set_power(None)
    _wait_power(sim, hub, None)
    assert _power_header(sim) == hidden


@pytest.mark.parametrize("reading,percent", [
    ({"battery_mv": 3300}, 0),
    ({"battery_mv": 4200}, 100),
    ({"battery_mv": 2000}, 0),
    ({"battery_mv": 4500}, 100),
    ({"battery_mv": 1999}, None),
    ({"battery_mv": 4501}, None),
    ({}, None),
    ({"battery_mv": None}, None),
    ({"battery_mv": 3840, "battery_present": False}, None),
    ({"battery_mv": 3840, "battery_present": None}, 50),
    ({"battery_mv": 3840, "battery_present": True}, 50),
    ({"battery_mv": 3840, "charging": True}, 50),
    ({"battery_mv": 3750, "battery_percent": 0}, 0),
    ({"battery_mv": 3750, "battery_percent": 100}, 100),
])
def test_power_boundaries_and_native_precedence(devserver, make_sim, reading, percent):
    hub, _brain, url = devserver()
    sim = _power_sim(make_sim, url)
    _wait_power(sim, hub, None)
    hidden = _power_header(sim)
    sim.console("set battery_estimate liion")
    sim.set_power(reading)
    _wait_power(sim, hub, reading)
    actual = _power_header(sim)
    if percent is None:
        assert actual == hidden
    else:
        assert actual != hidden
        reference = {**reading, "battery_percent": percent}
        sim.set_power(reference)
        _wait_power(sim, hub, reference)
        if reading.get("charging"):
            # A voltage estimate on a charger shows the icon alone; a native percentage keeps its number.
            assert _power_header(sim) != actual
        else:
            assert _power_header(sim) == actual
    # Full replacements must not leave the prior percentage, voltage or flags.
    sim.set_power({})
    _wait_power(sim, hub, {})
    assert _power_header(sim) == hidden


def test_power_validation_is_atomic_in_host_native_and_abi(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = _power_sim(make_sim, url)
    original = {"battery_present": True, "battery_mv": 3750, "battery_percent": 50,
                "charging": False, "external_power": True}
    sim.set_power(original)
    original["battery_mv"] = 4200
    expected = {**original, "battery_mv": 3750}
    assert sim.power_status == expected  # Caller mutation cannot change the fixture.
    _wait_power(sim, hub, expected)
    header = _power_header(sim)
    invalid = [
        [], "null", 0, True, {"unknown": 1}, {"battery_present": 1},
        {"charging": "yes"}, {"external_power": 0}, {"battery_mv": True},
        {"battery_mv": 3750.0}, {"battery_mv": "3750"}, {"battery_mv": -1},
        {"battery_mv": 65536}, {"battery_percent": False},
        {"battery_percent": 50.0}, {"battery_percent": -1}, {"battery_percent": 101},
        {"battery_mv": 4200, "charging": 1},
    ]
    for setter in (sim.set_power, sim.device.set_power):
        for reading in invalid:
            with pytest.raises(ValueError):
                setter(reading)
            assert sim.power_status == expected
    lib = sim.device._lib
    handle = sim.device._handle
    valid = (1, 1, 3750, 50, 0, 1)
    for index, bad in [(0, -1), (0, 2), (1, 2), (2, -2), (2, 65536),
                       (3, -2), (3, 101), (4, 2), (5, 2)]:
        args = list(valid)
        args[index] = bad
        assert lib.hgsim_set_power(handle, *args) == 0
    assert lib.hgsim_set_power(None, *valid) == 0
    # Even a clear request validates all fields before discarding the reading.
    assert lib.hgsim_set_power(handle, 0, 1, 3750, 101, 0, 1) == 0
    sim.run_for(5.2)
    _wait_power(sim, hub, expected)
    assert _power_header(sim) == header
    # ABI endpoints and explicit unknowns are accepted, not confused with rejection.
    for reading in ({"battery_mv": 0, "battery_percent": 0},
                    {"battery_mv": 65535, "battery_percent": 100},
                    {key: None for key in _POWER_FIELDS}):
        sim.set_power(reading)
        _wait_power(sim, hub, reading)


def test_power_settings_survive_restart_but_fixture_does_not(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = _power_sim(make_sim, url, state="power-persistence")
    sim.console("set battery_estimate liion")
    reading = {"battery_mv": 3750}
    sim.set_power(reading)
    _wait_power(sim, hub, reading)
    estimated = _power_header(sim)
    sim.close()
    # Do not reapply either console setting on the new production App.
    restarted = make_sim(url, board="sim-240x240", name="Waveshare 1.54",
                         state="power-persistence")
    assert restarted.wait_screen("ready", timeout=10)
    assert restarted.power_status is None
    _wait_power(restarted, hub, None)
    assert _power_header(restarted) != estimated
    restarted.set_power(reading)
    _wait_power(restarted, hub, reading)
    assert _power_header(restarted) == estimated


def test_device_reconnects_after_server_restart(loop_thread, devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    hub.port = hub.bound_port  # come back on the same port
    loop_thread.run(hub.stop())
    assert sim.wait_screen("connecting", timeout=5)
    loop_thread.run(hub.start())
    assert sim.wait_screen("ready", timeout=15)


def test_screens_render_deterministically(devserver, make_sim, tmp_path):
    hub, _brain, url = devserver()
    a = make_sim(url, state="a", name="Same")
    b = make_sim(url, state="b", name="Same")
    for sim in (a, b):
        assert sim.wait_screen("ready", timeout=10)
        sim.type_text("render me")
    for sim in (a, b):
        assert sim.wait_for(lambda s=sim: s.last_received("turn.end") is not None, timeout=10)
        assert sim.wait_screen("ready", timeout=5)
    # Same model -> same pixels; the animated header band differs by frame, so compare the body.
    top = 22 + 35
    assert a.rgb888(top, a.board.height - 22) == b.rgb888(top, b.board.height - 22)
    a.screenshot(tmp_path / "ready.png")
    assert (tmp_path / "ready.png").read_bytes().startswith(b"\x89PNG")


@pytest.mark.parametrize("board", ["sim-240x135", "sim-480x320", "sim-240x240-nospeaker"])
def test_other_board_profiles_work(devserver, make_sim, board):
    hub, _brain, url = devserver()
    sim = make_sim(url, board=board)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert session.display["width"] == sim.board.width
    assert session.has_speaker == sim.board.speaker
    sim.type_text("hi")
    assert sim.wait_for(lambda: sim.last_received("turn.end") is not None, timeout=10)


def test_hello_declares_capabilities(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    hello = next(m for m in sim.sent if m.get("type") == "hello")
    assert hello["proto"] == 1
    assert hello["caps"]["display"]["image"]["format"] == "rgb565"
    assert hello["caps"]["mic"] == {"rate": 16000, "format": "pcm16"}
    assert json.dumps(hello)  # serialisable


def test_holding_cancel_starts_a_new_session(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    sim.press("cancel")
    sim.run_for(2.3)
    sim.release("cancel")
    assert any(m.get("type") == "session.new" for m in sim.sent)
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "Started a new conversation.",
                        timeout=5)


@pytest.mark.parametrize("board, centre", [("sim-466x466-round", 233), ("sim-360x360-round", 180),
                                           ("sim-240x240-round", 120)])
def test_round_touch_board_talks_with_the_screen(devserver, make_sim, board, centre):
    hub, _brain, url = devserver()
    sim = make_sim(url, board=board)
    assert sim.wait_screen("ready", timeout=10)
    hello = next(m for m in sim.sent if m["type"] == "hello")
    assert hello["caps"]["display"]["shape"] == "round" and "touch" in hello["caps"]["inputs"]
    sim.touch(True, centre, centre)
    assert sim.wait_screen("listening", timeout=2)
    sim.run_for(0.6)
    sim.touch(False)
    assert sim.wait_for(lambda: any(m["type"] == "audio.end" for m in sim.sent), timeout=5)
    # Board settings ride on the same console as the core ones.
    assert sim.console("set touch_cancel pwr") == "@ok touch_cancel"


def _update(loop_thread, sim, coro_fn):
    """Run a firmware update on the hub's loop while the simulated device keeps stepping."""
    import asyncio

    fut = asyncio.run_coroutine_threadsafe(coro_fn(), loop_thread.loop)
    assert sim.wait_for(fut.done, timeout=60)
    return fut


def test_firmware_update_installs_and_the_device_comes_back(loop_thread, devserver, make_sim):
    from fakes.fake_firmware import fake_image

    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    device_id = sim.status()["device_id"]
    session = hub.get(device_id)
    assert session.caps["ota"]["max_size"] == 0x1F0000
    image = fake_image(board=sim.board.name, version="0.2.0", size=300_000)
    progress = []

    fut = _update(loop_thread, sim, lambda: session.update_firmware(image, progress=lambda s, t: progress.append(s)))
    assert fut.result() == "0.2.0"
    assert sim.update_image == image
    assert progress[-1] == len(image)
    assert sim.wait_for(lambda: sim.restarts == 1, timeout=5)  # after ota.done has left
    assert sim.wait_for(lambda: hub.get(device_id) not in (None, session), timeout=15)  # it reconnected
    assert sim.wait_screen("ready", timeout=10)


def test_firmware_update_refusals(loop_thread, devserver, make_sim):
    import hashlib

    from fakes.fake_firmware import fake_image
    from hermes_gadget_plugin import ota

    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = hub.get(sim.status()["device_id"])

    # Only the key the device enrolled can authorize an image.
    image = ota.inspect_image(fake_image(board=sim.board.name))
    fut = _update(loop_thread, sim, lambda: ota.send_update(session, image, bytes(32)))
    assert isinstance(fut.exception(), ota.UpdateError) and fut.exception().code == "unauthorized"
    assert sim.update_image is None

    # An image for another board never leaves the host.
    fut = _update(loop_thread, sim, lambda: session.update_firmware(fake_image(board="esp32s3-breadboard")))
    assert fut.exception().code == "wrong_board"

    # The device's own image check (here: the simulator's) has the last word.
    bad = bytearray(fake_image(board=sim.board.name))
    bad[0] = 0x00
    not_an_app = ota.FirmwareImage(bytes(bad), "0.2.0", sim.board.name, hashlib.sha256(bad).hexdigest())
    fut = _update(loop_thread, sim, lambda: ota.send_update(session, not_an_app, hub.store.key_for(session.device_id)))
    assert fut.exception().code == "invalid"
    assert sim.update_image is None and sim.restarts == 0
    assert sim.wait_screen("ready", timeout=5)


def test_a_new_firmware_is_kept_once_it_reaches_hermes(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url, update_pending=True)
    assert sim.wait_screen("ready", timeout=10)
    assert sim.update_confirmed


@pytest.mark.parametrize("board", ["sim-320x240", "sim-240x135", "sim-466x466-round"])
def test_the_dashboard_opens_from_the_idle_screen(loop_thread, devserver, make_sim, board, tmp_path):
    from hermes_gadget.devserver import SAMPLE_DASHBOARD
    from hermes_gadget_plugin import dashboard

    hub, _brain, url = devserver()
    sim = make_sim(url, board=board)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert session.dashboard_version == 1
    msg = dashboard.message(SAMPLE_DASHBOARD["sessions"], SAMPLE_DASHBOARD["tiles"],
                            cols=session.display.get("text_cols"))
    assert loop_thread.run(session.send_dashboard(msg))
    assert sim.wait_for(lambda: sim.last_received("dashboard") is not None, timeout=5)
    assert sim.status()["screen"] == "ready"  # it waits to be opened
    if sim.board.touch:
        sim.touch(True, sim.board.width // 2, sim.board.height // 3)
        for dy in range(0, 120, 20):
            sim.touch(True, sim.board.width // 2, sim.board.height // 3 + dy)
            sim.run_for(0.02)
        sim.touch(False)
    else:
        sim.tap("cancel")
    assert sim.wait_screen("dashboard", timeout=5)
    sim.run_for(0.2)
    rgb = sim.rgb888()
    pixels = {rgb[i:i + 3] for i in range(0, len(rgb), 3)}
    assert any(p[0] < 120 and 140 < p[1] < 200 and p[2] > 200 for p in pixels)  # the busy session's blue
    assert any(p[0] > 200 and 150 < p[1] < 200 and p[2] < 90 for p in pixels)  # the waiting one's amber
    sim.screenshot(tmp_path / f"dashboard-{board}.png")


class _TwoProfiles(HubDelegate):
    """Offers two profiles; switching always succeeds for known ones."""

    def profiles(self, session=None):
        return [{"id": "default", "name": "Hermes"}, {"id": "ops", "name": "Ops"}]

    async def select_profile(self, session, profile):
        if profile not in ("default", "ops"):
            return "unknown"
        session.profile = profile
        return None


def test_devices_switch_hermes_profile_and_keep_it(loop_thread, tmp_path, make_sim):
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    hub = DeviceHub(DeviceStore(tmp_path / "server"), _TwoProfiles(), host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    try:
        url = f"ws://127.0.0.1:{hub.bound_port}/gadget"
        sim = make_sim(url)
        assert sim.wait_screen("ready", timeout=10)
        welcome = sim.last_received("welcome")
        assert welcome["profile"] == "default"
        assert [p["id"] for p in welcome["profiles"]] == ["default", "ops"]
        sim.console("settings")
        for _ in range(3):
            sim.console("cancel")
        sim.console("talk")
        sim.console("release")
        assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
        assert hub.get(sim.status()["device_id"]).profile == "ops"
        assert sim.last_received("profile") == {"type": "profile", "profile": "ops"}  # the wire, not only state
        first_session = welcome["session"]
        sim.close()
        again = make_sim(url)  # same state dir: the device remembers its profile
        assert again.wait_screen("ready", timeout=10)
        assert again.last_received("welcome")["session"] != first_session
        assert again.last_received("welcome")["profile"] == "ops"
    finally:
        loop_thread.run(hub.stop())


def test_a_server_without_profiles_sends_no_roster(devserver, make_sim):
    # Server side of compatibility; old firmware against a capable adapter is in test_profile_select_hermes.py.
    hub, _brain, url = devserver()  # EchoBrain keeps the default hooks: no profiles
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    welcome = sim.last_received("welcome")
    assert "profile" not in welcome and "profiles" not in welcome
    assert hub.get(sim.status()["device_id"]).profile == "default"
