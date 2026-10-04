"""The simulated device: production core + simulated peripherals.

``Simulator`` is UI-agnostic. The Tk window drives it interactively; tests and
``hermes-gadget sim --headless`` drive it from code or a script.
"""

from __future__ import annotations

import json
import logging
import os
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .. import png
from .audio_io import Microphone, Speaker, load_wav
from .native import BUTTONS, NativeDevice
from .transport import WsTransport

log = logging.getLogger("hermes_gadget.sim")

FIRMWARE_VERSION = "0.1.0-sim"
UPDATE_SLOT_BYTES = 0x1F0000  # an app slot on the ESP32 boards (firmware/esp32/partitions.csv)


@dataclass
class Board:
    """A simulated board profile (screen geometry and peripherals)."""

    name: str
    width: int
    height: int
    mic: bool = True
    speaker: bool = True
    backlight: bool = True
    scroll_buttons: bool = True
    round: bool = False  # circular panel: pixels outside the circle are not shown
    touch: bool = False  # touchscreen: hold to talk, tap to answer yes, swipe down to cancel


BOARDS = {
    "sim-320x240": Board("sim-320x240", 320, 240),
    "sim-240x135": Board("sim-240x135", 240, 135),
    "sim-480x320": Board("sim-480x320", 480, 320),
    "sim-240x240-nospeaker": Board("sim-240x240-nospeaker", 240, 240, speaker=False),
    # A 1.54" 240x240 SPI LCD with codecs (e.g. Waveshare ESP32-S3-LCD-1.54): no scroll buttons.
    "sim-240x240": Board("sim-240x240", 240, 240, scroll_buttons=False),
    # A 1.75" round 466x466 AMOLED touch board (e.g. ESP32-S3-Touch-AMOLED-1.75): no scroll buttons.
    "sim-466x466-round": Board("sim-466x466-round", 466, 466, scroll_buttons=False, round=True, touch=True),
}


def _circle_spans(width: int, height: int) -> list[tuple[int, int]]:
    r = min(width, height) / 2
    spans = []
    for y in range(height):
        dy = y + 0.5 - height / 2
        half = (r * r - dy * dy) ** 0.5 if abs(dy) < r else 0.0
        spans.append((max(0, round(width / 2 - half)), min(width, round(width / 2 + half))))
    return spans


class JsonStorage:
    """The device's NVS, persisted to a JSON file so the device key survives restarts."""

    def __init__(self, path: Path | None):
        self.path = path
        self.data: dict[str, str] = {}
        if path and path.exists():
            try:
                self.data = json.loads(path.read_text(encoding="utf-8"))
            except ValueError:
                self.data = {}

    def get(self, key: str) -> str | None:
        return self.data.get(key)

    def set(self, key: str, value: str) -> None:
        self.data[key] = value
        self._save()

    def erase(self, key: str) -> None:
        if self.data.pop(key, None) is not None:
            self._save()

    def _save(self) -> None:
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)


@dataclass
class VirtualPeripherals:
    """Demo peripherals exposed to the agent as device actions."""

    led: str = "off"
    buzzer_count: int = 0
    backlight: int = 100
    volume: int = 70
    battery: float = 87.0
    temperature_c: float = 21.5
    changed: Callable[[], None] = field(default=lambda: None)


class Simulator:
    def __init__(
        self,
        *,
        url: str = "",
        board: str = "sim-320x240",
        name: str = "Sim Gadget",
        token: str = "",
        state_dir: Path | None = None,
        live_audio: bool = False,
        library: Path | None = None,
        button_labels: tuple[str, str] | None = None,  # e.g. ("TALK", "CANCEL") to match hardware
        update_pending: bool = False,  # boot as if running an update that isn't confirmed yet
    ):
        if board not in BOARDS:
            raise ValueError(f"unknown board {board!r}; choose from {', '.join(BOARDS)}")
        self.board = BOARDS[board]
        self.state_dir = state_dir
        self.storage = JsonStorage(state_dir / "nvs.json" if state_dir else None)
        # Command-line settings win over what the device stored last time.
        if url:
            self.storage.set("server", url)
        if token:
            self.storage.set("token", token)
        self.transport = WsTransport()
        self.mic = Microphone(live=live_audio)
        self.speaker = Speaker(state_dir / "audio" if state_dir else None, live=live_audio)
        self.peripherals = VirtualPeripherals()
        self.logs: list[tuple[int, str]] = []
        self.sent: list[dict] = []          # outbound JSON, newest last (for tests/UI)
        self.received: list[dict] = []      # inbound JSON
        self.on_flush: Callable[[int, int], None] | None = None
        self.on_log: Callable[[int, str], None] | None = None
        self._dirty_rows: tuple[int, int] | None = None
        self._t0 = time.monotonic()
        self._wav_release_at: float | None = None
        self.network_up = False
        self.update_image: bytes | None = None  # the last firmware image the device installed
        self.update_confirmed = False
        self.restarts = 0
        self._update_buf: bytearray | None = None
        self._restart_requested = False

        b = self.board
        self.device = NativeDevice(
            self, width=b.width, height=b.height, board=b.name, firmware=FIRMWARE_VERSION, name=name,
            server_url=url, access_token=token, mic=b.mic, speaker=b.speaker, backlight=b.backlight,
            scroll_buttons=b.scroll_buttons, library=library, button_labels=button_labels,
            round_panel=b.round, touch_screen=b.touch, update_capacity=UPDATE_SLOT_BYTES,
            update_pending=update_pending)
        self._round_spans = _circle_spans(b.width, b.height) if b.round else None
        self._register_actions()

    # -- Host implementation (called by the core) ---------------------------------------

    def transport_connect(self, url: str, subprotocol: str) -> None:
        self.transport.connect(url, subprotocol)

    def transport_send_text(self, text: str) -> bool:
        try:
            self.sent.append(json.loads(text))
            del self.sent[:-200]
        except ValueError:
            pass
        return self.transport.send(text)

    def transport_send_binary(self, data: bytes) -> bool:
        return self.transport.send(data)

    def transport_close(self) -> None:
        self.transport.close()

    def display_flush(self, y0: int, y1: int) -> None:
        if self._dirty_rows is None:
            self._dirty_rows = (y0, y1)
        else:
            self._dirty_rows = (min(self._dirty_rows[0], y0), max(self._dirty_rows[1], y1))
        if self.on_flush:
            self.on_flush(y0, y1)

    def display_backlight(self, percent: int) -> None:
        self.peripherals.backlight = percent
        self.peripherals.changed()

    def mic_start(self, rate: int) -> bool:
        return self.mic.start(rate) if self.board.mic else False

    def mic_stop(self) -> None:
        self.mic.stop()

    def speaker_begin(self, rate: int) -> bool:
        return self.speaker.begin(rate)

    def speaker_write(self, pcm: bytes) -> None:
        self.speaker.write(pcm)

    def speaker_end(self) -> None:
        self.speaker.end()

    def speaker_abort(self) -> None:
        self.speaker.abort()

    def speaker_busy(self) -> bool:
        return self.speaker.busy()

    def speaker_volume(self, percent: int) -> None:
        self.speaker.volume = percent
        self.peripherals.volume = percent
        self.peripherals.changed()

    def storage_get(self, key: str) -> str | None:
        return self.storage.get(key)

    def storage_set(self, key: str, value: str) -> None:
        self.storage.set(key, value)

    def storage_erase(self, key: str) -> None:
        self.storage.erase(key)

    def now_ms(self) -> int:
        return int((time.monotonic() - self._t0) * 1000)

    def random_bytes(self, n: int) -> bytes:
        return os.urandom(n)

    def log(self, level: int, message: str) -> None:
        self.logs.append((level, message))
        del self.logs[:-500]
        log.log({0: logging.DEBUG, 1: logging.INFO, 2: logging.WARNING}.get(level, logging.ERROR), "device: %s", message)
        if self.on_log:
            self.on_log(level, message)

    def update_begin(self, size: int) -> bool:
        self._update_buf = bytearray()
        return True

    def update_write(self, data: bytes) -> bool:
        if self._update_buf is None:
            return False
        self._update_buf += data
        return True

    def update_finish(self) -> bool:
        image, self._update_buf = self._update_buf, None
        if not image or image[0] != 0xE9:  # like the ESP32 port, take only app images
            return False
        self.update_image = bytes(image)
        if self.state_dir:
            self.state_dir.mkdir(parents=True, exist_ok=True)
            (self.state_dir / "update.bin").write_bytes(self.update_image)
        return True

    def update_abort(self) -> None:
        self._update_buf = None

    def update_restart(self) -> None:
        self._restart_requested = True  # done in step(), outside the core's tick

    def update_confirm(self) -> None:
        self.update_confirmed = True

    # -- virtual peripherals -------------------------------------------------------------

    def _register_actions(self) -> None:
        p = self.peripherals

        def led_set(args: dict) -> dict:
            color = str(args.get("color") or "").strip().lower()
            if not color:
                raise ValueError("color is required (a name like 'red' or a hex value like '#ff8800', or 'off')")
            p.led = color
            p.changed()
            return {"led": p.led}

        def buzzer_beep(args: dict) -> dict:
            times = max(1, min(5, int(args.get("times") or 1)))
            p.buzzer_count += times
            p.changed()
            return {"beeped": times}

        self.device.add_action(
            "led.set", "Set the colour of the gadget's status LED ('off' turns it off).",
            {"type": "object", "properties": {"color": {"type": "string", "description": "Colour name, #rrggbb, or 'off'"}},
             "required": ["color"]},
            led_set)
        self.device.add_action(
            "buzzer.beep", "Beep the gadget's buzzer to get someone's attention.",
            {"type": "object", "properties": {"times": {"type": "integer", "minimum": 1, "maximum": 5}}},
            buzzer_beep)

    # -- driving the device ----------------------------------------------------------------

    def start(self, network: bool = True) -> None:
        self.device.begin()
        self.device.set_sensor("battery_pct", round(self.peripherals.battery))
        self.device.set_sensor("temperature_c", self.peripherals.temperature_c)
        self.set_network(network)

    def set_network(self, up: bool) -> None:
        self.network_up = up
        self.device.network(up, "sim-wifi" if up else "Wi-Fi disconnected")

    def step(self) -> None:
        """Advance the device once: deliver transport events, feed the mic, tick."""
        while True:
            try:
                ev = self.transport.events.get_nowait()
            except Exception:
                break
            if ev.generation != self.transport.generation:
                continue
            if ev.kind == "open":
                self.device.transport_open()
            elif ev.kind == "text":
                try:
                    self.received.append(json.loads(ev.data))
                    del self.received[:-200]
                except ValueError:
                    pass
                self.device.transport_text(ev.data)
            elif ev.kind == "binary":
                self.device.transport_binary(ev.data)
            elif ev.kind == "closed":
                self.device.transport_closed(ev.data or "closed")
        pcm = self.mic.read()
        if pcm:
            self.device.mic_samples(pcm)
        if self._wav_release_at is not None and time.monotonic() >= self._wav_release_at:
            self._wav_release_at = None
            self.release("talk")
        self.device.tick()
        if self._restart_requested:
            self._restart_requested = False
            self._restart()

    def _restart(self) -> None:
        """A reboot as Hermes sees it: the connection drops, then the device connects again."""
        self.restarts += 1
        up = self.network_up
        self.set_network(False)
        if up:
            self.set_network(True)

    def run_for(self, seconds: float, interval: float = 0.01) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.step()
            time.sleep(interval)

    def wait_for(self, predicate: Callable[[], bool], timeout: float = 10.0, interval: float = 0.01) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.step()
            if predicate():
                return True
            time.sleep(interval)
        return False

    def wait_screen(self, *names: str, timeout: float = 10.0) -> bool:
        return self.wait_for(lambda: self.device.screen() in names, timeout)

    def press(self, button: str) -> None:
        self.device.button(BUTTONS[button], True)

    def release(self, button: str) -> None:
        self.device.button(BUTTONS[button], False)

    def touch(self, touching: bool, x: int = 0, y: int = 0) -> None:
        """A touchscreen sample in screen pixels (touch boards only)."""
        self.device.touch(touching, x, y)

    def tap(self, button: str) -> None:
        self.press(button)
        self.release(button)

    def type_text(self, text: str) -> None:
        self.device.submit_text(text)

    def speak_wav(self, path: str | Path, tail_s: float = 0.3) -> float:
        """Hold TALK, play a WAV file into the microphone, release when it ends."""
        pcm = load_wav(path, 16000)
        self.mic.inject(pcm)
        self.press("talk")
        seconds = len(pcm) / 2 / 16000
        self._wav_release_at = time.monotonic() + seconds + tail_s
        return seconds

    def speak_pcm(self, pcm: bytes, rate: int = 16000, tail_s: float = 0.3) -> float:
        self.mic.inject(pcm)
        self.press("talk")
        seconds = len(pcm) / 2 / rate
        self._wav_release_at = time.monotonic() + seconds + tail_s
        return seconds

    def set_sensor(self, name: str, value: float) -> None:
        self.device.set_sensor(name, value)

    def console(self, line: str) -> str:
        return self.device.console(line)

    def status(self) -> dict:
        return self.device.status()

    def last_received(self, type_: str) -> dict | None:
        for msg in reversed(self.received):
            if msg.get("type") == type_:
                return msg
        return None

    # -- framebuffer ---------------------------------------------------------------------

    def take_dirty_rows(self) -> tuple[int, int] | None:
        rows, self._dirty_rows = self._dirty_rows, None
        return rows

    def rgb888(self, y0: int = 0, y1: int | None = None) -> bytes:
        rgb = png.rgb565_to_rgb888(self.device.framebuffer_rows(y0, y1))
        if self._round_spans is None:
            return rgb
        # A round panel has no corners: show them black, as the glass would.
        out, w = bytearray(rgb), self.board.width
        for i, y in enumerate(range(y0, y0 + len(rgb) // (3 * w))):
            x0, x1 = self._round_spans[y]
            row = i * w * 3
            out[row: row + x0 * 3] = bytes(x0 * 3)
            out[row + x1 * 3: row + w * 3] = bytes((w - x1) * 3)
        return bytes(out)

    def screenshot(self, path: str | Path) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(png.encode_png(self.rgb888(), self.board.width, self.board.height))
        return path

    def close(self) -> None:
        self.mic.stop()
        self.speaker.abort()
        self.transport.shutdown()
        self.device.close()

    def drift_sensors(self) -> None:
        """Slow, plausible sensor changes so telemetry has something to report."""
        p = self.peripherals
        p.battery = max(5.0, p.battery - 0.02)
        p.temperature_c = round(p.temperature_c + random.uniform(-0.05, 0.05), 2)
        self.device.set_sensor("battery_pct", round(p.battery))
        self.device.set_sensor("temperature_c", p.temperature_c)
