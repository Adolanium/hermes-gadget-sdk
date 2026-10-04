"""Persistent device identity and a single-threaded production core host."""

from __future__ import annotations

import base64
import json
import logging
import os
import queue
import re
import tempfile
import time
from collections import deque
from pathlib import Path
from urllib.parse import urlsplit

from .. import __version__
from ..sim.native import BUTTONS, NativeDevice
from ..sim.transport import WsTransport


def load_config(path: Path) -> dict:
    config = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or set(config) - {"server", "name", "token", "audio", "gpio", "display"}:
        raise ValueError("unknown configuration field; expected server, name, token, audio, gpio or display")
    for key in {"server", "name", "token"} & config.keys():
        value = config[key]
        if not isinstance(value, str) or len(value.encode()) > 500 or "\x00" in value:
            raise ValueError(f"invalid {key}: expected a string of at most 500 bytes")
    url = urlsplit(config.get("server", ""))
    if url.scheme not in {"ws", "wss"} or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("server must be a ws:// or wss:// URL without credentials or a fragment")
    audio = config.get("audio", {})
    if not isinstance(audio, dict) or set(audio) - {"input", "output", "rate"}:
        raise ValueError("audio accepts input, output and rate")
    for key in {"input", "output"} & audio.keys():
        value = audio[key]
        if not ((type(value) is int and value >= 0) or (isinstance(value, str) and value.strip())):
            raise ValueError(f"audio.{key} must be a device number or name")
    if type(audio.get("rate", 16000)) is not int or audio.get("rate", 16000) not in (8000, 16000, 24000, 32000, 44100, 48000):
        raise ValueError("audio.rate must be 8000, 16000, 24000, 32000, 44100 or 48000")
    gpio = config.get("gpio", {})
    if not isinstance(gpio, dict) or set(gpio) - {"talk", "cancel", "status_led", "outputs", "chip"}:
        raise ValueError("gpio accepts talk, cancel, status_led, outputs and chip")
    outputs = gpio.get("outputs", {})
    if not isinstance(outputs, dict) or any(not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", name) for name in outputs):
        raise ValueError("GPIO output names must start with a letter and use lowercase letters, digits or underscores")
    pins = [gpio[key] for key in ("talk", "cancel", "status_led") if key in gpio] + list(outputs.values())
    if any(type(pin) is not int or not 2 <= pin <= 27 for pin in pins) or len(set(pins)) != len(pins):
        raise ValueError("GPIO pins must be distinct BCM numbers from 2 to 27")
    if type(gpio.get("chip", 0)) is not int or not 0 <= gpio.get("chip", 0) <= 15:
        raise ValueError("gpio.chip must be a number from 0 to 15")
    if "display" in config:
        display = config["display"]
        if not isinstance(display, dict) or set(display) - {"width", "height", "fullscreen", "rotation", "touch", "round"}:
            raise ValueError("display accepts width, height, fullscreen, rotation, touch and round")
        width, height = display.get("width", 320), display.get("height", 240)
        if any(type(size) is not int or size % 2 or not 160 <= size <= 800 for size in (width, height)):
            raise ValueError("display width and height must be even numbers from 160 to 800")
        if type(display.get("rotation", 0)) is not int or display.get("rotation", 0) not in (0, 90, 180, 270):
            raise ValueError("display rotation must be 0, 90, 180 or 270")
        for key in ("fullscreen", "touch", "round"):
            if key in display and not isinstance(display[key], bool):
                raise ValueError(f"display.{key} must be boolean")
        if display.get("round") and width != height:
            raise ValueError("a round display needs equal width and height")
    return config


class State:
    def __init__(self, directory: Path):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if os.name == "posix":
            directory.chmod(0o700)
        self.path = directory / "device.json"
        self.data = json.loads(self.path.read_text(encoding="utf-8")) if self.path.exists() else {}
        if not isinstance(self.data, dict) or any(
            not isinstance(k, str) or not isinstance(v, str) for k, v in self.data.items()
        ):
            raise ValueError("device.json is invalid; restore the saved device state")
        if "device_key" in self.data:
            key = base64.b64decode(self.data["device_key"], validate=True)
            if len(key) != 32:
                raise ValueError("invalid saved device key; restore the saved device state")

    def save(self) -> None:
        fd, name = tempfile.mkstemp(prefix=".device-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as file:
                json.dump(self.data, file)
                file.flush()
                os.fsync(file.fileno())
            os.replace(name, self.path)
            if os.name == "posix":
                directory = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
        finally:
            if os.path.exists(name):
                os.unlink(name)


class Client:
    def __init__(self, config: dict, state_dir: Path, *, library: Path | None = None):
        self.state = State(state_dir)
        self.failure: Exception | None = None
        self.started = time.monotonic()
        self.messages: deque[dict] = deque(maxlen=20)
        self.sequence = 0
        # Configuration is authoritative on every start; identity stays in device.json.
        self.state.data.update(server=config["server"], name=config.get("name", "Linux Gadget"),
                               token=config.get("token", ""))
        self.state.save()
        from .audio import Audio

        audio = config.get("audio", {})
        self.audio = Audio(audio) if "input" in audio or "output" in audio else None
        self.gpio = None
        self.display = None
        self.running = True
        if "display" in config:
            from .display import Display

            self.display = Display(config["display"])
        try:
            self.device = NativeDevice(
                self, width=self.display.width if self.display else 0,
                height=self.display.height if self.display else 0, board="linux", firmware=__version__,
                name=config.get("name", "Linux Gadget"), mic="input" in audio, speaker="output" in audio,
                mic_rate=audio.get("rate", 16000), speaker_rate=audio.get("rate", 16000), audio_host=self.audio,
                backlight=False, scroll_buttons=self.display is not None, library=library,
                button_labels=("TALK", "CANCEL"), touch_screen=self.display.touch if self.display else False,
                round_panel=self.display.round if self.display else False)
        except Exception:
            if self.display:
                self.display.close()
            raise
        self.transport = WsTransport()
        try:
            if config.get("gpio"):
                from .gpio import Gpio

                self.gpio = Gpio(config["gpio"], self.device)
        except Exception:
            self.close()
            raise

    def start(self) -> None:
        self.device.begin()
        self._check_storage()
        self.device.network(True, "Linux network")

    def step(self) -> None:
        self._check_storage()
        if self.display:
            self.running = self.display.poll(self.device)
        # Bound each iteration so traffic cannot starve controls or the core clock.
        for _ in range(64):
            try:
                event = self.transport.events.get_nowait()
            except queue.Empty:
                break
            if event.generation != self.transport.generation:
                continue
            if event.kind == "open":
                self.device.transport_open()
            elif event.kind == "text":
                self.device.transport_text(event.data)
                try:
                    message = json.loads(event.data)
                except ValueError:
                    continue
                if isinstance(message, dict) and message.get("type") in {
                    "reply", "transcript", "prompt", "turn.end", "notice", "error"
                }:
                    self.sequence += 1
                    entry = {key: value[:4096] for key, value in message.items()
                             if key in {"type", "text", "title", "message", "outcome", "code"}
                             and isinstance(value, str)}
                    self.messages.append({"sequence": self.sequence, **entry})
            elif event.kind == "binary":
                self.device.transport_binary(event.data)
            elif event.kind == "closed":
                self.device.transport_closed(event.data or "closed")
        if self.gpio:
            self.gpio.step(self.now_ms(), self.device.status())
        if self.audio:
            pcm = self.audio.read()
            if "input" in self.audio.errors:
                if self.device.screen() == "listening":
                    self.device.button(BUTTONS["cancel"], True)
                    self.device.button(BUTTONS["cancel"], False)
            elif pcm:
                self.device.mic_samples(pcm)
        self.device.tick()
        if self.display:
            self.display.present(self.device, self.now_ms())
        self._check_storage()

    def close(self) -> None:
        try:
            if self.gpio:
                self.gpio.close()
        finally:
            try:
                if self.audio:
                    self.audio.close()
            finally:
                self.transport.shutdown()
                self.device.close()
                if self.display:
                    self.display.close()

    def command(self, request: dict) -> dict:
        if not isinstance(request, dict):
            raise TypeError("request must be an object")
        command = request.get("command")
        if command == "status":
            status = {**self.device.status(), "uptime_s": int(time.monotonic() - self.started)}
            if self.audio:
                status["audio"] = {**self.audio.devices, "errors": dict(self.audio.errors)}
            return status
        if command == "messages":
            return {"messages": list(self.messages)}
        if command == "button":
            button, pressed = request.get("button"), request.get("pressed")
            if not isinstance(button, str) or button not in BUTTONS or not isinstance(pressed, bool):
                raise ValueError("button must be talk, cancel, up or down; pressed must be boolean")
            self.device.button(BUTTONS[button], pressed)
            return {"ok": True}
        if command not in {"send", "event"}:
            raise ValueError("unknown command")
        status = self.device.status()
        if status.get("phase") != "online" or not status.get("paired"):
            raise ValueError("device is not paired and online; check status")
        if command == "send":
            text = request.get("text")
            if not isinstance(text, str) or not text.strip() or len(text.encode()) > 2000 or "\x00" in text:
                raise ValueError("text must contain 1 to 2000 UTF-8 bytes")
            self.device.submit_text(text)
        else:
            name, data, notify = request.get("name"), request.get("data", {}), request.get("notify", False)
            if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_.-]{0,63}", name):
                raise ValueError("invalid event name")
            if not isinstance(data, dict) or len(json.dumps(data, allow_nan=False).encode()) > 4096:
                raise ValueError("event data must be an object of at most 4096 bytes")
            if not isinstance(notify, bool):
                raise ValueError("notify must be boolean")
            self.device.emit_event(name, data, notify)
        return {"ok": True}

    def _check_storage(self) -> None:
        if self.failure:
            raise RuntimeError("device state could not be saved; stopping") from self.failure

    def storage_get(self, key: str) -> str | None:
        return self.state.data.get(key)

    def storage_set(self, key: str, value: str) -> None:
        self.state.data[key] = value
        try:
            self.state.save()
        except OSError as exc:
            self.failure = exc
            raise

    def storage_erase(self, key: str) -> None:
        self.state.data.pop(key, None)
        try:
            self.state.save()
        except OSError as exc:
            self.failure = exc
            raise

    def transport_connect(self, url: str, subprotocol: str) -> None:
        self.transport.connect(url, subprotocol)

    def display_flush(self, y0: int, y1: int) -> None:
        self.display.dirty = True

    def transport_send_text(self, text: str) -> bool:
        return self.transport.send(text)

    def transport_send_binary(self, data: bytes) -> bool:
        return self.transport.send(data)

    def transport_close(self) -> None:
        self.transport.close()

    def now_ms(self) -> int:
        return int((time.monotonic() - self.started) * 1000)

    def random_bytes(self, n: int) -> bytes:
        return os.urandom(n)

    def log(self, level: int, message: str) -> None:
        # The core logs connection state and action failures, never device keys.
        logging.getLogger("hermes_gadget.linux").log((logging.DEBUG, logging.INFO, logging.WARNING,
                                                     logging.ERROR)[level], message)
