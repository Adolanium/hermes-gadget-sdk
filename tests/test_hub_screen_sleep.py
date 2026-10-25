"""While a device's screen is dark the hub holds back cards, images and status phrases.

Only the latest of each goes out when the screen lights up. Questions, replies and
everything else still go out at once (docs/protocol.md#screen-sleep).
"""

from __future__ import annotations

import base64
import json
import secrets
import time

import pytest

from hermes_gadget_plugin import protocol


class Device:
    """A bare protocol client: says what the firmware says, records what it receives."""

    def __init__(self, url: str, *, key: bytes | None = None, sleeping: bool | None = None, dashboard: bool = False):
        from websockets.sync.client import connect

        self.key = key or secrets.token_bytes(32)
        self.device_id = protocol.device_id_for_key(self.key)
        self.ws = connect(url, subprotocols=[protocol.SUBPROTOCOL], open_timeout=5)
        hello = protocol.message("hello", proto=protocol.VERSION, device_id=self.device_id, name="watch",
                                 board="sim-320x240", caps={"display": {"width": 320, "height": 240,
                                                                        "image": {"width": 64, "height": 64,
                                                                                  "format": "rgb565"}}})
        if sleeping is not None:
            hello["display_sleeping"] = sleeping
        if dashboard:
            hello["caps"]["dashboard"] = {"v": 1}
        self.ws.send(json.dumps(hello))
        challenge = json.loads(self.ws.recv(timeout=5))
        if challenge["enrolled"]:
            mac = protocol.auth_mac(self.key, self.device_id, challenge["nonce"])
            self.ws.send(json.dumps(protocol.message("auth", mac=mac)))
        else:
            self.ws.send(json.dumps(protocol.message("auth", key=base64.b64encode(self.key).decode())))
        assert json.loads(self.ws.recv(timeout=5))["type"] == "welcome"

    def screen(self, sleeping: bool) -> None:
        self.ws.send(json.dumps(protocol.message("state", display_sleeping=sleeping)))

    def frames(self, quiet_s: float = 0.3) -> list:
        """Everything received until the line has been quiet for `quiet_s`: dicts, or bytes for
        binary. Heartbeat pings are left out."""
        out = []
        while True:
            try:
                raw = self.ws.recv(timeout=quiet_s)
            except TimeoutError:
                return out
            frame = raw if isinstance(raw, bytes) else json.loads(raw)
            if not (isinstance(frame, dict) and frame.get("type") == "ping"):
                out.append(frame)

    def close(self) -> None:
        self.ws.close()


def types(frames: list) -> list[str]:
    return [f["type"] if isinstance(f, dict) else "binary" for f in frames]


@pytest.fixture
def online(devserver, loop_thread):
    hub, _brain, url = devserver()
    devices = []

    def connect(**kw) -> tuple[Device, object]:
        device = Device(url, **kw)
        devices.append(device)
        assert _wait(lambda: device.device_id in hub.sessions)
        return device, hub.sessions[device.device_id]

    yield hub, connect
    for d in devices:
        d.close()


def _wait(predicate, timeout: float = 5.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.01)
    return False


def test_a_lit_screen_gets_updates_at_once(online, loop_thread):
    hub, connect = online
    device, session = connect()
    assert session.display_sleeping is False
    assert loop_thread.run(session.show_card("Timer", "9 min", ttl_s=0)) is True
    assert loop_thread.run(session.send_status("Searching")) is True
    assert types(device.frames()) == ["display", "status"]


def test_firmware_that_never_reports_the_screen_is_never_held_back(online, loop_thread):
    hub, connect = online
    device, session = connect()  # no display_sleeping in its hello, and no state about it
    device.ws.send(json.dumps(protocol.message("state", sensors={"battery_pct": 50})))
    assert _wait(lambda: session.sensors.get("battery_pct") == 50)
    assert loop_thread.run(session.show_card("Timer", "9 min")) is True
    assert types(device.frames()) == ["display"]


def test_only_the_latest_card_or_image_and_status_go_out_when_the_screen_lights(online, loop_thread):
    hub, connect = online
    device, session = connect()
    device.screen(True)
    assert _wait(lambda: session.display_sleeping)

    assert loop_thread.run(session.show_card("Old", "first card", ttl_s=0)) is False
    assert loop_thread.run(session.send_status("Thinking")) is False
    assert loop_thread.run(session.show_image(8, 8, b"\x00\xf8" * 64, ttl_s=0)) is False  # replaces the card
    assert loop_thread.run(session.show_card("Timer", "Pasta: 9 min", ttl_s=0)) is False  # replaces the image
    assert loop_thread.run(session.send_status("Searching the web")) is False
    assert device.frames() == [], "nothing is sent while the screen is dark"
    assert session.describe()["display_sleeping"] is True

    device.screen(False)
    got = device.frames()
    assert types(got) == ["status", "display"]
    assert got[0]["text"] == "Searching the web"
    assert got[1]["title"] == "Timer" and got[1]["body"] == "Pasta: 9 min" and got[1]["ttl_s"] == 0
    device.screen(True)
    device.screen(False)
    assert device.frames() == [], "what was held is sent once"


def test_a_held_image_is_sent_whole_when_it_is_the_latest(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    assert session.display_sleeping is True
    pixels = b"\x00\xf8" * (64 * 64)
    loop_thread.run(session.show_card("Old", "replaced"))
    assert loop_thread.run(session.show_image(64, 64, pixels, ttl_s=30)) is False
    assert device.frames() == []
    device.screen(False)
    got = device.frames()
    assert types(got) == ["image.start"] + ["binary"] * 2 + ["image.end"]
    assert got[0]["width"] == 64 and 29 <= got[0]["ttl_s"] <= 30
    assert sum(len(f) - protocol.BINARY_HEADER for f in got[1:3]) == len(pixels)


def test_questions_replies_and_notices_are_never_held(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    loop_thread.run(session.ask("q1", "Approve", "rm -rf build", ttl_s=60))
    loop_thread.run(session.turn_start("t1"))
    loop_thread.run(session.send_reply("Done", turn="t1"))
    loop_thread.run(session.send_notice("Heads up"))
    assert types(device.frames()) == ["prompt", "turn.start", "reply", "notice"]


def test_a_card_whose_time_ran_out_in_the_dark_is_dropped(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    session.display_sleeping = True
    loop_thread.run(session.show_card("Short", "gone soon", ttl_s=15))
    hub._held[session.device_id].held_at -= 10  # held for 10 s
    session.display_sleeping = False
    loop_thread.run(session.screen_woke())
    got = device.frames()
    assert types(got) == ["display"] and got[0]["ttl_s"] == 5, "it keeps only what is left of its time"

    session.display_sleeping = True
    loop_thread.run(session.show_card("Short", "gone soon", ttl_s=15))
    hub._held[session.device_id].held_at -= 16
    session.display_sleeping = False
    loop_thread.run(session.screen_woke())
    assert device.frames() == []


def test_a_status_phrase_from_a_finished_turn_is_dropped(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    loop_thread.run(session.send_status("Searching the web"))
    loop_thread.run(session.turn_end("t1"))
    assert types(device.frames()) == ["turn.end"]
    device.screen(False)
    assert device.frames() == []


def test_what_was_held_survives_a_reconnect_and_goes_out_once_the_screen_is_lit(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    loop_thread.run(session.show_card("Timer", "Pasta: 9 min", ttl_s=0))
    device.close()
    assert _wait(lambda: device.device_id not in hub.sessions)

    dark, _ = connect(key=device.key, sleeping=True)  # Wi-Fi came back, screen still dark
    assert dark.frames() == []
    dark.close()
    assert _wait(lambda: device.device_id not in hub.sessions)

    lit, _ = connect(key=device.key)  # woken while offline
    got = lit.frames()
    assert types(got) == ["display"] and got[0]["title"] == "Timer"


def test_the_simulated_firmware_holds_a_card_until_its_screen_wakes(devserver, make_sim, loop_thread):
    """End to end with the C++ core: the timeout darkens the screen, TALK lights it."""
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert sim.console("set screen_timeout 1").startswith("@ok")
    assert sim.wait_for(lambda: session.display_sleeping, timeout=5)
    assert sim.peripherals.backlight == 0

    assert loop_thread.run(session.show_card("Timer", "Pasta: 9 minutes", ttl_s=0)) is False
    sim.run_for(0.3)
    assert sim.last_received("display") is None
    assert sim.peripherals.backlight == 0, "a held card doesn't light the screen"

    sim.tap("talk")  # only wakes the screen
    assert sim.wait_for(lambda: not session.display_sleeping, timeout=5)
    assert sim.wait_screen("card", timeout=5)
    assert sim.last_received("display")["title"] == "Timer"


def test_the_simulated_firmware_sleeps_face_down(devserver, make_sim):
    """End to end with the C++ core: lying face down sleeps the screen, turning it over wakes it."""
    hub, _brain, url = devserver()
    sim = make_sim(url, board="sim-466x466-round")
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert sim.console("set face_down_sleep on").startswith("@ok")
    sim.set_face_down(True)
    assert sim.wait_for(lambda: sim.status()["face_down"], timeout=5)
    assert sim.peripherals.backlight == 0
    assert sim.wait_for(lambda: session.display_sleeping, timeout=5)

    sim.set_face_down(False)
    assert sim.wait_for(lambda: not sim.status()["face_down"], timeout=5)
    assert sim.peripherals.backlight > 0
    assert sim.wait_for(lambda: not session.display_sleeping, timeout=5)

    plain = make_sim(url, state="no-motion")
    assert plain.wait_screen("ready", timeout=10)
    assert "face_down" not in plain.status()


def test_the_simulated_firmware_rests_on_a_clock(devserver, make_sim):
    """End to end with the C++ core: after a rest the round board shows the clock at 70% of the
    brightness setting; a tap brings Ready back at full brightness without recording."""
    hub, _brain, url = devserver()
    sim = make_sim(url, board="sim-466x466-round")
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    sim.set_wall_time(time.strptime("2026-10-24T09:41", "%Y-%m-%dT%H:%M"))
    assert sim.console("set talk_mode tap").startswith("@ok")  # a tap on Ready would record
    assert sim.console("set clock 5").startswith("@ok")
    assert sim.wait_screen("clock", timeout=10)
    assert sim.peripherals.backlight == 70
    assert not session.display_sleeping  # a lit screen: Hermes still sends what it has

    sim.touch(True, 233, 233)
    sim.run_for(0.05)
    sim.touch(False, 233, 233)
    assert sim.wait_screen("ready", timeout=5)
    sim.run_for(0.5)
    assert sim.status()["screen"] == "ready", "the tap only brought Ready back"
    assert sim.peripherals.backlight == 100
