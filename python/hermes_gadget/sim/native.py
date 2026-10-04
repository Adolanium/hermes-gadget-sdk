"""ctypes binding to the simulator library (firmware/sim/include/hgsim.h).

The library contains the production device core; this module only translates
between C callbacks and a Python "host" object that plays the drivers.
"""

from __future__ import annotations

import ctypes
import json
import logging
from ctypes import (
    CFUNCTYPE, POINTER, Structure, c_char, c_char_p, c_double, c_int, c_size_t, c_uint8, c_uint16,
    c_uint32, c_void_p, c_int16,
)
from pathlib import Path
from typing import Any, Callable, Protocol

from .. import paths

log = logging.getLogger("hermes_gadget.sim")

ABI_VERSION = 5

BUTTON_TALK, BUTTON_CANCEL, BUTTON_UP, BUTTON_DOWN = 0, 1, 2, 3
BUTTONS = {"talk": BUTTON_TALK, "cancel": BUTTON_CANCEL, "up": BUTTON_UP, "down": BUTTON_DOWN}

_TRANSPORT_CONNECT = CFUNCTYPE(None, c_void_p, c_char_p, c_char_p)
_SEND_TEXT = CFUNCTYPE(c_int, c_void_p, POINTER(c_char), c_size_t)
_SEND_BINARY = CFUNCTYPE(c_int, c_void_p, POINTER(c_uint8), c_size_t)
_VOID = CFUNCTYPE(None, c_void_p)
_FLUSH = CFUNCTYPE(None, c_void_p, c_int, c_int)
_INT_ARG = CFUNCTYPE(None, c_void_p, c_int)
_START = CFUNCTYPE(c_int, c_void_p, c_uint32)
_SPK_WRITE = CFUNCTYPE(None, c_void_p, POINTER(c_int16), c_size_t)
_INT_RET = CFUNCTYPE(c_int, c_void_p)
_STORAGE_GET = CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_char), c_size_t)
_STORAGE_SET = CFUNCTYPE(None, c_void_p, c_char_p, c_char_p)
_STORAGE_ERASE = CFUNCTYPE(None, c_void_p, c_char_p)
_NOW = CFUNCTYPE(c_uint32, c_void_p)
_RANDOM = CFUNCTYPE(None, c_void_p, POINTER(c_uint8), c_size_t)
_LOG = CFUNCTYPE(None, c_void_p, c_int, c_char_p)
_ACTION = CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_char), c_size_t)
_UPDATE_BEGIN = CFUNCTYPE(c_int, c_void_p, c_size_t)
_UPDATE_WRITE = CFUNCTYPE(c_int, c_void_p, POINTER(c_uint8), c_size_t)


class _Host(Structure):
    _fields_ = [
        ("user", c_void_p),
        ("transport_connect", _TRANSPORT_CONNECT),
        ("transport_send_text", _SEND_TEXT),
        ("transport_send_binary", _SEND_BINARY),
        ("transport_close", _VOID),
        ("display_flush", _FLUSH),
        ("display_backlight", _INT_ARG),
        ("mic_start", _START),
        ("mic_stop", _VOID),
        ("speaker_begin", _START),
        ("speaker_write", _SPK_WRITE),
        ("speaker_end", _VOID),
        ("speaker_abort", _VOID),
        ("speaker_busy", _INT_RET),
        ("speaker_volume", _INT_ARG),
        ("storage_get", _STORAGE_GET),
        ("storage_set", _STORAGE_SET),
        ("storage_erase", _STORAGE_ERASE),
        ("now_ms", _NOW),
        ("random_bytes", _RANDOM),
        ("log", _LOG),
        ("update_begin", _UPDATE_BEGIN),
        ("update_write", _UPDATE_WRITE),
        ("update_finish", _INT_RET),
        ("update_abort", _VOID),
        ("update_restart", _VOID),
        ("update_confirm", _VOID),
    ]


class _Config(Structure):
    _fields_ = [
        ("width", c_int),
        ("height", c_int),
        ("has_mic", c_int),
        ("has_speaker", c_int),
        ("has_backlight", c_int),
        ("has_scroll_buttons", c_int),
        ("mic_rate", c_uint32),
        ("speaker_rate", c_uint32),
        ("board", c_char_p),
        ("firmware", c_char_p),
        ("default_name", c_char_p),
        ("default_server_url", c_char_p),
        ("default_access_token", c_char_p),
        ("talk_label", c_char_p),
        ("cancel_label", c_char_p),
        ("round", c_int),
        ("touch", c_int),
        ("update_capacity", c_size_t),
        ("update_pending", c_int),
    ]


class Host(Protocol):
    """What the simulator must provide in place of real drivers."""

    def transport_connect(self, url: str, subprotocol: str) -> None: ...
    def transport_send_text(self, text: str) -> bool: ...
    def transport_send_binary(self, data: bytes) -> bool: ...
    def transport_close(self) -> None: ...
    def display_flush(self, y0: int, y1: int) -> None: ...
    def display_backlight(self, percent: int) -> None: ...
    def mic_start(self, rate: int) -> bool: ...
    def mic_stop(self) -> None: ...
    def speaker_begin(self, rate: int) -> bool: ...
    def speaker_write(self, pcm: bytes) -> None: ...
    def speaker_end(self) -> None: ...
    def speaker_abort(self) -> None: ...
    def speaker_busy(self) -> bool: ...
    def speaker_volume(self, percent: int) -> None: ...
    def storage_get(self, key: str) -> str | None: ...
    def storage_set(self, key: str, value: str) -> None: ...
    def storage_erase(self, key: str) -> None: ...
    def now_ms(self) -> int: ...
    def random_bytes(self, n: int) -> bytes: ...
    def log(self, level: int, message: str) -> None: ...
    def update_begin(self, size: int) -> bool: ...
    def update_write(self, data: bytes) -> bool: ...
    def update_finish(self) -> bool: ...
    def update_abort(self) -> None: ...
    def update_restart(self) -> None: ...
    def update_confirm(self) -> None: ...


def _guard(default):
    """Exceptions must never unwind into C."""
    def wrap(fn):
        def inner(*args):
            try:
                return fn(*args)
            except Exception:
                log.exception("simulator callback %s failed", getattr(fn, "__name__", fn))
                return default
        return inner
    return wrap


class SimLibraryError(RuntimeError):
    pass


def load_library(path: Path | None = None) -> ctypes.CDLL:
    lib_path = path or paths.find_sim_library()
    if lib_path is None or not Path(lib_path).exists():
        raise SimLibraryError(
            "simulator library not found; build it with 'hermes-gadget build-sim' "
            "(or set HGSIM_LIBRARY to the built hgsim library)")
    lib = ctypes.CDLL(str(lib_path))
    lib.hgsim_abi_version.restype = c_int
    if lib.hgsim_abi_version() != ABI_VERSION:
        raise SimLibraryError(f"{lib_path} has ABI {lib.hgsim_abi_version()}, expected {ABI_VERSION}; rebuild it")
    lib.hgsim_create.restype = c_void_p
    lib.hgsim_create.argtypes = [POINTER(_Config), POINTER(_Host)]
    lib.hgsim_destroy.argtypes = [c_void_p]
    lib.hgsim_add_action.argtypes = [c_void_p, c_char_p, c_char_p, c_char_p, _ACTION, c_void_p]
    lib.hgsim_add_action.restype = c_int
    for name in ("hgsim_begin", "hgsim_tick", "hgsim_transport_open"):
        getattr(lib, name).argtypes = [c_void_p]
    lib.hgsim_network.argtypes = [c_void_p, c_int, c_char_p]
    lib.hgsim_transport_text.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.hgsim_transport_binary.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.hgsim_transport_closed.argtypes = [c_void_p, c_char_p]
    lib.hgsim_button.argtypes = [c_void_p, c_int, c_int]
    lib.hgsim_touch.argtypes = [c_void_p, c_int, c_int, c_int]
    lib.hgsim_mic_samples.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.hgsim_submit_text.argtypes = [c_void_p, c_char_p]
    lib.hgsim_set_sensor.argtypes = [c_void_p, c_char_p, c_double]
    lib.hgsim_emit_event.argtypes = [c_void_p, c_char_p, c_char_p, c_int]
    lib.hgsim_console.argtypes = [c_void_p, c_char_p, c_char_p, c_size_t]
    lib.hgsim_console.restype = c_int
    lib.hgsim_status.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.hgsim_status.restype = c_int
    lib.hgsim_screen.argtypes = [c_void_p]
    lib.hgsim_screen.restype = c_char_p
    lib.hgsim_framebuffer.argtypes = [c_void_p, POINTER(c_int), POINTER(c_int)]
    lib.hgsim_framebuffer.restype = POINTER(c_uint16)
    return lib


ActionHandler = Callable[[dict], dict]


class NativeDevice:
    """One simulated device instance running the production core."""

    def __init__(self, host: Host, *, width: int, height: int, board: str, firmware: str, name: str,
                 server_url: str = "", access_token: str = "", mic: bool = True, speaker: bool = True,
                 backlight: bool = True, scroll_buttons: bool = True, mic_rate: int = 16000,
                 speaker_rate: int = 16000, library: Path | None = None,
                 button_labels: tuple[str, str] | None = None, round_panel: bool = False,
                 touch_screen: bool = False, update_capacity: int = 0, update_pending: bool = False):
        self._lib = load_library(library)
        self._host_obj = host
        self.width, self.height = width, height
        self._strings = [s.encode() for s in (board, firmware, name, server_url, access_token)]
        self._strings += [s.encode() for s in button_labels] if button_labels else [None, None]
        self._config = _Config(width, height, int(mic), int(speaker), int(backlight), int(scroll_buttons),
                               mic_rate, speaker_rate, *self._strings, int(round_panel), int(touch_screen),
                               update_capacity, int(update_pending))
        self._callbacks = self._make_callbacks(host)
        self._handle = self._lib.hgsim_create(ctypes.byref(self._config), ctypes.byref(self._callbacks))
        if not self._handle:
            raise SimLibraryError("hgsim_create failed")
        self._action_refs: list[Any] = []

    def _make_callbacks(self, h: Host) -> _Host:
        @_guard(None)
        def transport_connect(_u, url, sub):
            h.transport_connect(url.decode(), sub.decode() if sub else "")

        @_guard(0)
        def send_text(_u, data, n):
            return int(bool(h.transport_send_text(ctypes.string_at(data, n).decode("utf-8", "replace"))))

        @_guard(0)
        def send_binary(_u, data, n):
            return int(bool(h.transport_send_binary(ctypes.string_at(data, n))))

        @_guard(0)
        def mic_start(_u, rate):
            return int(bool(h.mic_start(int(rate))))

        @_guard(0)
        def spk_begin(_u, rate):
            return int(bool(h.speaker_begin(int(rate))))

        @_guard(None)
        def spk_write(_u, samples, count):
            h.speaker_write(ctypes.string_at(samples, count * 2))

        @_guard(0)
        def spk_busy(_u):
            return int(bool(h.speaker_busy()))

        @_guard(-1)
        def storage_get(_u, key, out, cap):
            value = h.storage_get(key.decode())
            if value is None:
                return -1
            raw = value.encode()
            n = min(len(raw), cap - 1)
            ctypes.memmove(out, raw, n)
            out[n] = b"\x00"
            return len(raw)

        @_guard(None)
        def storage_set(_u, key, value):
            h.storage_set(key.decode(), value.decode())

        @_guard(None)
        def storage_erase(_u, key):
            h.storage_erase(key.decode())

        @_guard(0)
        def now_ms(_u):
            return int(h.now_ms()) & 0xFFFFFFFF

        @_guard(None)
        def random_bytes(_u, out, n):
            data = h.random_bytes(n)
            ctypes.memmove(out, data, n)

        @_guard(None)
        def log_cb(_u, level, msg):
            h.log(int(level), msg.decode("utf-8", "replace"))

        @_guard(0)
        def update_begin(_u, size):
            return int(bool(h.update_begin(int(size))))

        @_guard(0)
        def update_write(_u, data, n):
            return int(bool(h.update_write(ctypes.string_at(data, n))))

        @_guard(0)
        def update_finish(_u):
            return int(bool(h.update_finish()))

        return _Host(
            None,
            _TRANSPORT_CONNECT(transport_connect),
            _SEND_TEXT(send_text),
            _SEND_BINARY(send_binary),
            _VOID(_guard(None)(lambda _u: h.transport_close())),
            _FLUSH(_guard(None)(lambda _u, y0, y1: h.display_flush(y0, y1))),
            _INT_ARG(_guard(None)(lambda _u, p: h.display_backlight(p))),
            _START(mic_start),
            _VOID(_guard(None)(lambda _u: h.mic_stop())),
            _START(spk_begin),
            _SPK_WRITE(spk_write),
            _VOID(_guard(None)(lambda _u: h.speaker_end())),
            _VOID(_guard(None)(lambda _u: h.speaker_abort())),
            _INT_RET(spk_busy),
            _INT_ARG(_guard(None)(lambda _u, p: h.speaker_volume(p))),
            _STORAGE_GET(storage_get),
            _STORAGE_SET(storage_set),
            _STORAGE_ERASE(storage_erase),
            _NOW(now_ms),
            _RANDOM(random_bytes),
            _LOG(log_cb),
            _UPDATE_BEGIN(update_begin),
            _UPDATE_WRITE(update_write),
            _INT_RET(update_finish),
            _VOID(_guard(None)(lambda _u: h.update_abort())),
            _VOID(_guard(None)(lambda _u: h.update_restart())),
            _VOID(_guard(None)(lambda _u: h.update_confirm())),
        )

    # -- lifecycle --------------------------------------------------------------------

    def add_action(self, name: str, description: str, params: dict, handler: ActionHandler) -> None:
        @_guard(0)
        def call(_u, args_json, out, cap):
            try:
                result = handler(json.loads(args_json.decode() or "{}"))
                ok, payload = 1, json.dumps(result if isinstance(result, dict) else {"value": result})
            except Exception as exc:  # reported to the agent as the action error
                ok, payload = 0, str(exc) or "action failed"
            raw = payload.encode()[: cap - 1]
            ctypes.memmove(out, raw, len(raw))
            out[len(raw)] = b"\x00"
            return ok

        cb = _ACTION(call)
        self._action_refs.append(cb)
        if not self._lib.hgsim_add_action(self._handle, name.encode(), description.encode(),
                                          json.dumps(params).encode(), cb, None):
            raise ValueError(f"invalid action definition: {name}")

    def begin(self) -> None:
        self._lib.hgsim_begin(self._handle)

    def tick(self) -> None:
        self._lib.hgsim_tick(self._handle)

    def close(self) -> None:
        if self._handle:
            self._lib.hgsim_destroy(self._handle)
            self._handle = None

    # -- events -----------------------------------------------------------------------

    def network(self, up: bool, detail: str = "") -> None:
        self._lib.hgsim_network(self._handle, int(up), detail.encode())

    def transport_open(self) -> None:
        self._lib.hgsim_transport_open(self._handle)

    def transport_text(self, text: str) -> None:
        raw = text.encode()
        self._lib.hgsim_transport_text(self._handle, raw, len(raw))

    def transport_binary(self, data: bytes) -> None:
        self._lib.hgsim_transport_binary(self._handle, data, len(data))

    def transport_closed(self, reason: str) -> None:
        self._lib.hgsim_transport_closed(self._handle, reason.encode())

    def button(self, button: int, pressed: bool) -> None:
        self._lib.hgsim_button(self._handle, button, int(pressed))

    def touch(self, touching: bool, x: int = 0, y: int = 0) -> None:
        self._lib.hgsim_touch(self._handle, int(touching), int(x), int(y))

    def mic_samples(self, pcm: bytes) -> None:
        self._lib.hgsim_mic_samples(self._handle, pcm, len(pcm) // 2)

    def submit_text(self, text: str) -> None:
        self._lib.hgsim_submit_text(self._handle, text.encode())

    def set_sensor(self, name: str, value: float) -> None:
        self._lib.hgsim_set_sensor(self._handle, name.encode(), float(value))

    def emit_event(self, name: str, data: Any = None, notify: bool = False) -> None:
        self._lib.hgsim_emit_event(self._handle, name.encode(), json.dumps(data or {}).encode(), int(notify))

    # -- introspection ----------------------------------------------------------------

    def console(self, line: str) -> str:
        buf = ctypes.create_string_buffer(4096)
        self._lib.hgsim_console(self._handle, line.encode(), buf, len(buf))
        return buf.value.decode("utf-8", "replace")

    def status(self) -> dict:
        buf = ctypes.create_string_buffer(4096)
        self._lib.hgsim_status(self._handle, buf, len(buf))
        return json.loads(buf.value.decode() or "{}")

    def screen(self) -> str:
        return self._lib.hgsim_screen(self._handle).decode()

    def framebuffer_rows(self, y0: int = 0, y1: int | None = None) -> bytes:
        """Raw little-endian RGB565 for rows [y0, y1)."""
        w, h = c_int(), c_int()
        ptr = self._lib.hgsim_framebuffer(self._handle, ctypes.byref(w), ctypes.byref(h))
        y1 = h.value if y1 is None else y1
        base = ctypes.cast(ptr, c_void_p).value + y0 * w.value * 2
        return ctypes.string_at(base, (y1 - y0) * w.value * 2)
