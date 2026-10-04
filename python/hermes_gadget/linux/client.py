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
    if not isinstance(config, dict) or set(config) - {"server", "name", "token"}:
        raise ValueError("config must be an object containing server, name and optionally token")
    for key, value in config.items():
        if not isinstance(value, str) or len(value.encode()) > 500 or "\x00" in value:
            raise ValueError(f"invalid {key}: expected a string of at most 500 bytes")
    url = urlsplit(config.get("server", ""))
    if url.scheme not in {"ws", "wss"} or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("server must be a ws:// or wss:// URL without credentials or a fragment")
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
        self.device = NativeDevice(self, width=0, height=0, board="linux", firmware=__version__,
                                   name=config.get("name", "Linux Gadget"), mic=False, speaker=False,
                                   backlight=False, scroll_buttons=False, library=library)
        self.transport = WsTransport()

    def start(self) -> None:
        self.device.begin()
        self._check_storage()
        self.device.network(True, "Linux network")

    def step(self) -> None:
        self._check_storage()
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
        self.device.tick()
        self._check_storage()

    def close(self) -> None:
        self.transport.shutdown()
        self.device.close()

    def command(self, request: dict) -> dict:
        if not isinstance(request, dict):
            raise TypeError("request must be an object")
        command = request.get("command")
        if command == "status":
            return {**self.device.status(), "uptime_s": int(time.monotonic() - self.started)}
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
