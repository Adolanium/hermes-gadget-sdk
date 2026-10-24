"""The production device core (in the simulator) against the production hub."""

import json
import math
import struct
import threading
import weakref

import pytest

from conftest import requires_sim
from hermes_gadget_plugin.face import ProfileFace
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


def test_welcome_names_the_language_and_an_auto_device_follows_it(devserver, make_sim):
    _hub, _brain, url = devserver(language="es")
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    welcome = [m for m in sim.received if m.get("type") == "welcome"][-1]
    assert welcome["language"] == "es"
    assert sim.status()["language"] == "en"  # English by default: welcome doesn't change it
    assert sim.console("set language auto") == "@ok language"
    assert sim.status()["language"] == "es" and sim.status()["language_auto"] is True


def test_welcome_names_no_language_when_the_server_has_none(devserver, make_sim):
    _hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    welcome = [m for m in sim.received if m.get("type") == "welcome"][-1]
    assert "language" not in welcome
    assert sim.console("set language auto") == "@ok language"
    assert sim.status()["language"] == "en"


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


def _face(ink: int) -> bytes:
    """A face the device takes (docs/protocol.md#faces): every bitmap byte ``ink``, the mascot's anchors."""
    from hermes_gadget_plugin.face import FACE_BYTES

    anchors = struct.pack("<10h", 607, 293, 262, 620, 801, 107, 396, 508, 586, 649)
    return bytes([ink]) * (FACE_BYTES - len(anchors)) + anchors


def _digest(face: bytes) -> str:
    return ProfileFace.of(face).hash


def _held_face(sim):
    """The hash of the face the device holds, as the hello of a reconnect names it."""
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_screen("ready", timeout=10)
    return [m for m in sim.sent if m.get("type") == "hello"][-1].get("face")


def _shows_mascot(sim, session, loop_thread) -> bool:
    """Whether the screen shows the mascot: clearing the face then changes nothing but a blink."""
    sim.run_for(0.2)
    before = sim.rgb888()
    loop_thread.run(session.send_json({"type": "face", "profile": session.device_profile}))
    sim.run_for(0.2)
    after = sim.rgb888()
    return sum(before[i:i + 3] != after[i:i + 3] for i in range(0, len(before), 3)) < 300


class _ProfileFaces(_TwoProfiles):
    """The default profile has a face; ops has none."""

    async def face(self, session):
        return ProfileFace.of(_face(0xF0)) if session.profile == "default" else None


def test_a_profile_face_is_sent_once_and_the_mascot_comes_back_on_a_switch(loop_thread, tmp_path, make_sim):
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    hub = DeviceHub(DeviceStore(tmp_path / "server"), _ProfileFaces(), host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    faces = lambda: [m for m in sim.received if m.get("type") == "face"]  # noqa: E731
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: faces(), timeout=5)
        sent = faces()[0]
        assert sent["profile"] == "default" and len(sent["hash"]) == 16 and sent["bits"]

        sim.console("reconnect")  # the device names the face it holds, so it is not sent again
        assert sim.wait_screen("connecting", timeout=5)
        assert sim.wait_screen("ready", timeout=10)
        sim.run_for(0.3)
        assert len(faces()) == 1

        device = hub.get(sim.status()["device_id"])
        loop_thread.run(hub.delegate.select_profile(device, "ops"))
        loop_thread.run(device.send_profile())  # ops has no face: the device drops it by itself
        assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
        sim.run_for(0.3)
        assert len(faces()) == 1 and device.face_hash is None

        loop_thread.run(hub.delegate.select_profile(device, "default"))
        loop_thread.run(device.send_profile())
        assert sim.wait_for(lambda: len(faces()) == 2, timeout=5)
        assert faces()[1]["hash"] == sent["hash"]
    finally:
        loop_thread.run(hub.stop())


class _RoutedLater(_TwoProfiles):
    """Two profiles a device picks from, until a gadget route sends it to ``route``: no roster then."""

    def __init__(self):
        self.route = None
        self.faces = {"ops": _face(0x0F), "work": _face(0x3C)}
        self.converting = None  # a threading.Event the route's face waits for, as converting an avatar would
        self.started = threading.Event()  # set once that wait begins

    def profiles(self, session=None):
        return [] if self.route else super().profiles(session)

    async def face(self, session):
        data = self.faces.get(self.route or session.profile)
        if data is None:
            return None
        if not (self.route and self.converting):
            return ProfileFace.of(data)

        def load():
            self.started.set()
            self.converting.wait(10)
            return data
        return ProfileFace(_digest(data), load)


def test_a_device_on_a_route_added_while_it_was_away_takes_the_routes_face(loop_thread, tmp_path, make_sim):
    """The welcome then names no profile, so a face names the one the device kept, and the device takes it."""
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    delegate = _RoutedLater()
    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    url = f"ws://127.0.0.1:{hub.bound_port}/gadget"
    last_face = lambda sim: ([m for m in sim.received if m.get("type") == "face"] or [{}])[-1]  # noqa: E731
    try:
        sim = make_sim(url)
        assert sim.wait_screen("ready", timeout=10)
        device = hub.get(sim.status()["device_id"])
        loop_thread.run(delegate.select_profile(device, "ops"))
        loop_thread.run(device.send_profile())
        assert sim.wait_for(lambda: last_face(sim).get("hash") == _digest(delegate.faces["ops"]), timeout=5)
        assert _held_face(sim) == _digest(delegate.faces["ops"])

        delegate.route = "work"  # a reconnect with ops's face held
        assert _held_face(sim) == _digest(delegate.faces["ops"])
        assert sim.wait_for(lambda: last_face(sim).get("hash") == _digest(delegate.faces["work"]), timeout=5)
        assert last_face(sim)["profile"] == "ops"  # the profile the device keeps, not the route's
        assert _held_face(sim) == _digest(delegate.faces["work"])

        work = _digest(delegate.faces.pop("work"))  # no face, or profile_faces off
        assert _held_face(sim) == work
        assert "face" not in sim.last_received("welcome")  # so the device drops it
        assert _held_face(sim) is None

        delegate.faces["work"] = _face(0x3C)
        sim.close()
        again = make_sim(url)  # a restart: the stored choice of ops, no face in memory
        assert again.wait_screen("ready", timeout=10)
        assert again.wait_for(lambda: last_face(again).get("hash") == work, timeout=5)
        assert _held_face(again) == work
    finally:
        loop_thread.run(hub.stop())


def test_a_new_route_shows_the_mascot_while_its_face_is_made(loop_thread, tmp_path, make_sim):
    """Never the previous profile's face: the welcome drops it at once, and the route's face follows."""
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    delegate = _RoutedLater()
    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    last_face = lambda: ([m for m in sim.received if m.get("type") == "face"] or [{}])[-1]  # noqa: E731
    ops, work = _digest(delegate.faces["ops"]), _digest(delegate.faces["work"])
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        device_id = sim.status()["device_id"]
        device = hub.get(device_id)
        loop_thread.run(delegate.select_profile(device, "ops"))
        loop_thread.run(device.send_profile())
        assert sim.wait_for(lambda: last_face().get("hash") == ops, timeout=5)
        assert _held_face(sim) == ops  # an unchanged face stays through a reconnect, unsent
        assert sim.last_received("welcome")["face"] == ops

        delegate.converting = threading.Event()
        delegate.route = "work"
        welcomes = len([m for m in sim.received if m.get("type") == "welcome"])
        sim.console("reconnect")
        assert sim.wait_for(delegate.started.is_set, timeout=10)  # work's face is being made
        assert _shows_mascot(sim, hub.get(device_id), loop_thread)  # not ops's face meanwhile
        delegate.converting.set()
        assert sim.wait_for(lambda: len([m for m in sim.received if m.get("type") == "welcome"]) > welcomes, timeout=10)
        assert "face" not in sim.last_received("welcome")  # ops's face is not kept
        assert sim.wait_for(lambda: last_face().get("hash") == work, timeout=5)
        assert _held_face(sim) == work
    finally:
        if delegate.converting:
            delegate.converting.set()  # no face left waiting on a failure
        loop_thread.run(hub.stop())


class _SlowLookup(_ProfileFaces):
    """Finding the face waits for ``found``, as a stalled file read would."""

    def __init__(self):
        self.found = None  # an asyncio.Event
        self.texts = []

    async def face(self, session):
        if self.found:
            await self.found.wait()
        return await super().face(session)

    async def on_text(self, session, msg_id, text):
        self.texts.append(text)


def test_a_stalled_face_lookup_does_not_hold_up_the_connection(loop_thread, tmp_path, make_sim):
    import asyncio

    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    delegate = _SlowLookup()
    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    try:
        delegate.found = asyncio.Event()  # never set: the lookup never finishes
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=8)  # well inside the device's 10 s handshake limit
        assert "face" not in sim.last_received("welcome")
        sim.type_text("still here?")  # messages are read while the face is still being looked up
        assert sim.wait_for(lambda: delegate.texts == ["still here?"], timeout=5)
        welcomes = len([m for m in sim.received if m.get("type") == "welcome"])
        sim.run_for(16)  # past the device's limit for a silent server: 3 heartbeats of 5 s
        assert sim.status()["phase"] == "online"
        assert len([m for m in sim.received if m.get("type") == "welcome"]) == welcomes  # no reconnect
    finally:
        loop_thread.loop.call_soon_threadsafe(delegate.found.set)
        loop_thread.run(hub.stop())


class _Picture:
    """Stands for the source picture a face's load() keeps."""


class _PictureFaces(_ProfileFaces):
    """Each face's load() keeps a picture, as a converted avatar's does; ``pictures`` tracks them."""

    def __init__(self):
        self.pictures = []

    async def face(self, session):
        picture, data = _Picture(), _face(0xF0)
        self.pictures.append(weakref.ref(picture))
        return ProfileFace(_digest(data), lambda: picture and data)


def test_a_connection_keeps_no_source_picture_once_its_face_is_settled(loop_thread, tmp_path, make_sim):
    import gc

    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    delegate = _PictureFaces()
    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: sim.last_received("face"), timeout=5)  # sent: the picture was used
        assert _held_face(sim) == _digest(_face(0xF0))  # kept: the picture was not needed
        sim.run_for(0.3)
        gc.collect()
        assert len(delegate.pictures) == 2 and all(ref() is None for ref in delegate.pictures)
    finally:
        loop_thread.run(hub.stop())


def test_a_device_unpaired_while_its_face_is_looked_up_gets_an_unpaired_welcome(loop_thread, tmp_path, make_sim):
    import asyncio

    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    delegate = _SlowLookup()
    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: sim.last_received("face"), timeout=5)
        device_id, first = sim.status()["device_id"], hub.get(sim.status()["device_id"])
        sent = len([m for m in sim.received if m.get("type") == "face"])
        delegate.found = asyncio.Event()
        sim.console("reconnect")
        assert sim.wait_for(lambda: hub.get(device_id) not in (None, first), timeout=10)
        loop_thread.run(hub.get(device_id).set_paired(False))  # revoked while the face is looked up
        loop_thread.loop.call_soon_threadsafe(delegate.found.set)
        assert sim.wait_for(lambda: sim.last_received("welcome")["session"] != first.session_id, timeout=10)
        assert sim.last_received("welcome")["paired"] is False
        sim.run_for(0.3)
        assert sim.status()["paired"] is False
        assert not any("bits" in m for m in [m for m in sim.received if m.get("type") == "face"][sent:])  # no face sent
    finally:
        if delegate.found:
            loop_thread.loop.call_soon_threadsafe(delegate.found.set)
        loop_thread.run(hub.stop())


def test_a_restarted_gateway_keeps_an_unchanged_face_without_sending_it(loop_thread, tmp_path, make_sim):
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    def start_hub(port=0):
        hub = DeviceHub(DeviceStore(tmp_path / "server"), _ProfileFaces(), host="127.0.0.1", port=port, heartbeat_s=5)
        loop_thread.run(hub.start())
        return hub

    hub = start_hub()
    port = hub.bound_port
    faces = lambda: [m for m in sim.received if m.get("type") == "face"]  # noqa: E731
    try:
        sim = make_sim(f"ws://127.0.0.1:{port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: faces(), timeout=5)
        held = faces()[0]["hash"]
        welcomes = len([m for m in sim.received if m.get("type") == "welcome"])
        loop_thread.run(hub.stop())
        hub = start_hub(port)  # a new process: it has never sent this device anything
        assert sim.wait_for(lambda: len([m for m in sim.received if m.get("type") == "welcome"]) > welcomes, timeout=15)
        assert sim.wait_screen("ready", timeout=5)
        assert sim.last_received("welcome")["face"] == held  # kept: no mascot at Ready
        sim.run_for(0.3)
        assert len(faces()) == 1  # and not sent again
        assert _held_face(sim) == held
    finally:
        loop_thread.run(hub.stop())


class _OnReady(_TwoProfiles):
    """The profiles in ``with_face`` have a face; ``on_ready`` can switch the device to another profile
    or unpair it, as the gateway may before the face work after the welcome has begun."""

    def __init__(self, with_face=("ops",)):
        self.with_face = with_face
        self.switch_to = None
        self.unpair = False

    async def face(self, session):
        return ProfileFace.of(_face(0x0F)) if session.profile in self.with_face else None

    async def on_ready(self, session):
        if self.switch_to:
            await self.select_profile(session, self.switch_to)
            await session.send_profile()
        if self.unpair:
            await session.set_paired(False)


def _hub(loop_thread, tmp_path, delegate):
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    hub = DeviceHub(DeviceStore(tmp_path / "server"), delegate, host="127.0.0.1", port=0, heartbeat_s=5)
    loop_thread.run(hub.start())
    return hub


def test_a_switch_before_the_face_work_starts_installs_no_old_face(loop_thread, tmp_path, make_sim):
    delegate = _OnReady()
    hub = _hub(loop_thread, tmp_path, delegate)
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        device = hub.get(sim.status()["device_id"])
        loop_thread.run(delegate.select_profile(device, "ops"))
        loop_thread.run(device.send_profile())
        assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("bits"), timeout=5)
        ops = _digest(_face(0x0F))
        assert _held_face(sim) == ops

        delegate.switch_to = "default"  # the next connection switches before its face work starts
        sim.console("reconnect")
        assert sim.wait_for(lambda: sim.status().get("profile") == "default", timeout=10)
        sim.run_for(0.5)
        delegate.switch_to = None
        assert _held_face(sim) is None  # default has no face: the mascot, not ops's face
    finally:
        loop_thread.run(hub.stop())


def test_an_unpair_before_the_face_work_starts_sends_no_face(loop_thread, tmp_path, make_sim):
    delegate = _OnReady(with_face=("default",))
    delegate.unpair = True  # after the welcome found default's face, before the face work starts
    hub = _hub(loop_thread, tmp_path, delegate)
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_for(lambda: sim.last_received("unpaired"), timeout=10)
        sim.run_for(0.5)
        assert not any("bits" in m for m in sim.received if m.get("type") == "face")
        assert sim.status()["paired"] is False
    finally:
        loop_thread.run(hub.stop())


def test_a_closed_connection_leaves_no_face_work_behind(loop_thread, tmp_path, make_sim):
    import asyncio
    import time

    delegate = _SlowLookup()
    hub = _hub(loop_thread, tmp_path, delegate)
    try:
        delegate.found = asyncio.Event()  # never set: the lookup never finishes
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=8)
        assert hub._tasks  # the face work, still looking
        sim.close()
        deadline = time.monotonic() + 5
        while (hub.sessions or hub._tasks) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not hub.sessions and not hub._tasks
    finally:
        loop_thread.loop.call_soon_threadsafe(delegate.found.set)
        loop_thread.run(hub.stop())


class _SlowOps(_TwoProfiles):
    """Finding ops's face waits for ``found``; texts are kept."""

    def __init__(self):
        self.found = None  # an asyncio.Event
        self.texts = []

    async def face(self, session):
        if session.profile == "ops":
            await self.found.wait()
        return None

    async def on_text(self, session, msg_id, text):
        self.texts.append(text)


def test_messages_are_read_while_a_switched_profiles_face_is_looked_up(loop_thread, tmp_path, make_sim):
    import asyncio

    delegate = _SlowOps()
    hub = _hub(loop_thread, tmp_path, delegate)
    try:
        delegate.found = asyncio.Event()  # never set
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        sim.console("settings")  # the device asks for ops, whose face never comes
        for _ in range(3):
            sim.console("cancel")
        sim.console("talk")
        sim.console("release")
        sim.console("settings close")
        assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
        sim.type_text("still here?")
        assert sim.wait_for(lambda: delegate.texts == ["still here?"], timeout=5)
    finally:
        loop_thread.loop.call_soon_threadsafe(delegate.found.set)
        loop_thread.run(hub.stop())


class _HeldLookup(_TwoProfiles):
    """Both profiles have a face. Arming ``hold`` (an asyncio.Event) holds the next lookup until it is set."""

    def __init__(self):
        self.faces = {"ops": _face(0x0F), "default": _face(0x3C)}
        self.hold = None
        self.waiting = False

    async def face(self, session):
        if self.hold is not None:
            hold, self.hold = self.hold, None
            self.waiting = True
            await hold.wait()
        return ProfileFace.of(self.faces[session.profile]) if session.profile in self.faces else None


def test_a_profile_reset_during_the_welcome_lookup_leaves_the_device_its_face(loop_thread, tmp_path, make_sim):
    """A reset sends the default face before the welcome, and the welcome then drops it: it is sent again."""
    import asyncio

    delegate = _HeldLookup()
    hub = _hub(loop_thread, tmp_path, delegate)
    default = _digest(delegate.faces["default"])
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        device_id = sim.status()["device_id"]
        loop_thread.run(delegate.select_profile(hub.get(device_id), "ops"))
        loop_thread.run(hub.get(device_id).send_profile())
        assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("profile") == "ops", timeout=5)

        hold = delegate.hold = asyncio.Event()
        welcomes = len([m for m in sim.received if m.get("type") == "welcome"])
        sim.console("reconnect")
        assert sim.wait_for(lambda: delegate.waiting, timeout=10)

        async def reset():  # as the plugin resets a stored profile it no longer serves
            session = hub.get(device_id)
            session.profile = "default"
            await session.send_profile()
        loop_thread.run(reset())
        assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == default, timeout=5)
        loop_thread.loop.call_soon_threadsafe(hold.set)
        assert sim.wait_for(lambda: len([m for m in sim.received if m.get("type") == "welcome"]) > welcomes, timeout=10)
        sim.run_for(0.5)
        assert _held_face(sim) == default
    finally:
        if delegate.hold:
            loop_thread.loop.call_soon_threadsafe(delegate.hold.set)
        loop_thread.run(hub.stop())


def test_an_unpair_and_a_quick_reapproval_leave_the_device_its_face(loop_thread, tmp_path, make_sim):
    import asyncio

    delegate = _HeldLookup()
    hub = _hub(loop_thread, tmp_path, delegate)
    default = _digest(delegate.faces["default"])
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == default, timeout=5)
        session = hub.get(sim.status()["device_id"])

        async def revoke_and_reapprove():  # before the face work of the revocation has run
            await session.set_paired(False)
            await session.set_paired(True)
            await asyncio.gather(session._face_task, return_exceptions=True)
        loop_thread.run(revoke_and_reapprove())
        assert sim.wait_for(lambda: sim.status()["paired"] is True, timeout=5)
        sim.run_for(0.5)
        assert _held_face(sim) == default
    finally:
        loop_thread.run(hub.stop())


def _announce_default_again(loop_thread, tmp_path, make_sim, remove_face):
    """Settles default's face, announces default again, and returns the face messages it caused and the
    face the device then holds. ``remove_face`` removes default's face file and avatar first."""
    delegate = _HeldLookup()
    hub = _hub(loop_thread, tmp_path, delegate)
    faces = lambda: [m for m in sim.received if m.get("type") == "face"]  # noqa: E731
    try:
        sim = make_sim(f"ws://127.0.0.1:{hub.bound_port}/gadget")
        assert sim.wait_screen("ready", timeout=10)
        assert sim.wait_for(lambda: faces() and faces()[-1].get("hash") == _digest(delegate.faces["default"]),
                            timeout=5)
        sent = len(faces())
        if remove_face:
            del delegate.faces["default"]
        loop_thread.run(hub.get(sim.status()["device_id"]).send_profile())  # the profile it is already on
        sim.run_for(0.5)
        return faces()[sent:], _held_face(sim)
    finally:
        loop_thread.run(hub.stop())


def test_announcing_the_same_profile_again_does_not_resend_its_face(loop_thread, tmp_path, make_sim):
    """The device keeps its face when the profile it is on is announced again, so the hub must too."""
    caused, held = _announce_default_again(loop_thread, tmp_path, make_sim, remove_face=False)
    assert caused == [] and held == _digest(_face(0x3C))


def test_announcing_the_same_profile_again_clears_a_face_whose_source_is_gone(loop_thread, tmp_path, make_sim):
    caused, held = _announce_default_again(loop_thread, tmp_path, make_sim, remove_face=True)
    assert [("bits" in m) for m in caused] == [False]  # one explicit clear
    assert held is None


def test_a_server_without_profiles_sends_no_roster(devserver, make_sim):
    # Server side of compatibility; old firmware against a capable adapter is in test_profile_select_hermes.py.
    hub, _brain, url = devserver()  # EchoBrain keeps the default hooks: no profiles
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    welcome = sim.last_received("welcome")
    assert "profile" not in welcome and "profiles" not in welcome
    assert hub.get(sim.status()["device_id"]).profile == "default"


def _timer_events(brain):
    """Records the device's events the hub passes to the brain; returns the list."""
    events = []

    async def on_event(session, name, data, notify):
        events.append((name, data, notify))

    brain.on_event = on_event
    return events


def test_timer_runs_and_rings_through_the_hub(loop_thread, devserver, make_sim):
    import asyncio

    hub, brain, url = devserver()
    events = _timer_events(brain)
    sim = make_sim(url, board="sim-466x466-round")
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert {"timer.start", "timer.status", "timer.cancel"} <= set(session.action_names())

    # The device answers on its own thread, so step the simulator while the hub waits.
    started = asyncio.run_coroutine_threadsafe(session.invoke_action("timer.start", {"seconds": 1, "label": "Pasta"}),
                                               loop_thread.loop)
    assert sim.wait_for(started.done, timeout=5)
    assert started.result() == {"seconds": 1, "label": "Pasta"}
    assert sim.wait_for(lambda: any(name == "timer.done" for name, _, _ in events), timeout=5)
    name, data, notify = next(e for e in events if e[0] == "timer.done")
    assert data == {"label": "Pasta"} and notify is False
    assert sim.wait_screen("alarm", timeout=3)

    sim.tap("talk")  # a press stops the alarm; it records nothing
    assert sim.wait_screen("ready", timeout=3)
    assert not any(m.get("type") == "audio.start" for m in sim.sent[-5:])


@pytest.mark.parametrize("board", ["sim-360x360-knob", "sim-480x480-round-nospeaker"])
def test_timer_on_a_board_without_a_speaker_shows_the_alarm_silently(loop_thread, devserver, make_sim, board):
    import asyncio

    hub, brain, url = devserver()
    events = _timer_events(brain)
    sim = make_sim(url, board=board)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert {"timer.start", "timer.status", "timer.cancel"} <= set(session.action_names())
    assert session.has_speaker is False

    started = asyncio.run_coroutine_threadsafe(session.invoke_action("timer.start", {"seconds": 1}), loop_thread.loop)
    assert sim.wait_for(started.done, timeout=5)
    assert sim.wait_screen("alarm", timeout=5)
    assert sim.wait_for(lambda: any(name == "timer.done" for name, _, _ in events), timeout=5)
