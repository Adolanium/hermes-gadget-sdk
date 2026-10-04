"""The production device core (in the simulator) against the production hub."""

import json
import math
import struct

import pytest

from conftest import requires_sim

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


def test_round_touch_board_talks_with_the_screen(devserver, make_sim):
    hub, _brain, url = devserver()
    sim = make_sim(url, board="sim-466x466-round")
    assert sim.wait_screen("ready", timeout=10)
    hello = next(m for m in sim.sent if m["type"] == "hello")
    assert hello["caps"]["display"]["shape"] == "round" and "touch" in hello["caps"]["inputs"]
    sim.touch(True, 233, 233)
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
