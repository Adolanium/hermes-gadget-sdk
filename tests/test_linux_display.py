"""The Linux screen renders native pixels and maps its controls through SDL."""

import json
import struct

import pytest
from conftest import requires_sim
from hermes_gadget.linux.client import Client, load_config
from hermes_gadget.linux.display import Display
from test_linux import wait

pytest.importorskip("pygame", reason="optional display extra is not installed")


@pytest.fixture(autouse=True)
def dummy_display(monkeypatch):
    monkeypatch.setenv("SDL_VIDEODRIVER", "dummy")
    monkeypatch.setenv("PYGAME_HIDE_SUPPORT_PROMPT", "1")


@pytest.mark.parametrize("rotation,corner", [(0, (0, 0)), (90, (319, 0)), (180, (319, 239)), (270, (0, 239))])
def test_display_pixels_rotation_and_touch_coordinates(rotation, corner):
    display = Display({"fullscreen": False, "rotation": rotation})
    try:
        class Frame:
            def framebuffer_rows(self):
                return struct.pack("<H", 0xF800) * (320 * 240)

        display.present(Frame(), 100)
        assert display.window.get_at(display.viewport.center)[:3] == (255, 0, 0)
        assert display.coordinates(display.viewport.topleft) == corner
        assert display.coordinates((-1, -1)) is None
    finally:
        display.close()


@requires_sim
def test_device_display_advertises_dimensions_and_renders_reply(devserver, tmp_path):
    hub, _, url = devserver()
    client = Client({"server": url, "display": {"fullscreen": False}}, tmp_path)
    try:
        client.start()
        wait(client, lambda: client.device.status()["paired"])
        session = next(iter(hub.sessions.values()))
        assert (session.display["width"], session.display["height"]) == (320, 240)
        assert "touch" in session.caps["inputs"]
        pg = client.display.pg
        before = pg.image.tobytes(client.display.window, "RGB")
        client.command({"command": "send", "text": "Hello screen"})
        wait(client, lambda: any(m.get("type") == "reply" for m in client.messages))
        assert next(m["text"] for m in client.messages if m["type"] == "reply") == "You said: Hello screen"
        client.display.dirty = True
        client.display.present(client.device, client.display.last_frame + 100)
        assert pg.image.tobytes(client.display.window, "RGB") != before
        assert len(client.device.framebuffer_rows()) == 153600
        pg.event.post(pg.event.Event(pg.QUIT))
        client.step()
        assert client.running is False
    finally:
        client.close()


def test_keyboard_repeat_and_lost_focus_release_controls():
    display = Display({"fullscreen": False})
    events = []

    class Device:
        def button(self, key, pressed):
            events.append((key, pressed))

    try:
        pg = display.pg
        pg.event.post(pg.event.Event(pg.KEYDOWN, key=pg.K_SPACE))
        pg.event.post(pg.event.Event(pg.KEYDOWN, key=pg.K_SPACE))
        pg.event.post(pg.event.Event(pg.WINDOWFOCUSLOST))
        assert display.poll(Device()) is True
        assert events == [(0, True), (0, False)]
    finally:
        display.close()


@pytest.mark.parametrize("display", [{"width": 321}, {"rotation": 45}, {"touch": 1}, {"round": True}, {"height": 0}])
def test_display_config_rejects_unsupported_dimensions_and_flags(display, tmp_path):
    path = tmp_path / "config.json"
    path.write_text(json.dumps({"server": "ws://localhost/gadget", "display": display}))
    with pytest.raises(ValueError):
        load_config(path)
