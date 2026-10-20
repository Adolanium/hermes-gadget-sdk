"""Opus encoding for speaker audio through the host's libopus (ctypes, no Python package).

Optional: without libopus the hub keeps sending PCM16. One encoder per stream; each call
encodes one frame of mono PCM16 into one packet, which goes out as one binary audio frame.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import logging
from typing import Optional

log = logging.getLogger(__name__)

APPLICATION_VOIP = 2048
SET_BITRATE_REQUEST = 4002
FRAME_MS = (2.5, 5, 10, 20, 40, 60)  # the frame durations Opus encodes
RATES = (8000, 12000, 16000, 24000, 48000)
MAX_PACKET = 1500

_lib: Optional[ctypes.CDLL] = None
_tried = False


def _load() -> Optional[ctypes.CDLL]:
    global _lib, _tried
    if _tried:
        return _lib
    _tried = True
    for name in (ctypes.util.find_library("opus"), "libopus.so.0", "libopus.dylib", "opus.dll"):
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            continue
        lib.opus_encoder_create.restype = ctypes.c_void_p
        lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        lib.opus_encode.restype = ctypes.c_int32
        lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int32]
        lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        # opus_encoder_ctl is variadic: declaring only the fixed arguments makes ctypes use
        # the variadic calling convention for the rest (it differs on Apple arm64).
        lib.opus_encoder_ctl.restype = ctypes.c_int
        lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
        _lib = lib
        break
    if _lib is None:
        log.info("libopus not found: gadgets get PCM16 speaker audio")
    return _lib


def available() -> bool:
    return _load() is not None


class Encoder:
    """Mono VoIP encoder at ``rate`` aiming at ``bitrate`` bits per second."""

    def __init__(self, rate: int, bitrate: int):
        lib = _load()
        if lib is None:
            raise RuntimeError("libopus is not available")
        if rate not in RATES:
            raise ValueError(f"Opus cannot encode at {rate} Hz")
        err = ctypes.c_int(0)
        state = lib.opus_encoder_create(rate, 1, APPLICATION_VOIP, ctypes.byref(err))
        if err.value != 0 or not state:
            raise RuntimeError(f"opus_encoder_create failed ({err.value})")
        self._lib, self._state, self.rate = lib, state, rate
        # The variadic value goes with an explicit C type; a refused bitrate fails the stream,
        # so the hub falls back to PCM16 instead of sending at a bitrate nobody asked for.
        rc = lib.opus_encoder_ctl(state, SET_BITRATE_REQUEST, ctypes.c_int32(int(bitrate)))
        if rc != 0:
            self.close()
            raise ValueError(f"Opus refused bitrate {bitrate} ({rc})")
        self._out = ctypes.create_string_buffer(MAX_PACKET)

    def encode(self, pcm: bytes) -> bytes:
        """One frame of PCM16 (a duration in FRAME_MS) in, one packet out."""
        samples = len(pcm) // 2
        n = self._lib.opus_encode(self._state, pcm, samples, self._out, MAX_PACKET)
        if n < 0:
            raise RuntimeError(f"opus_encode failed ({n})")
        return self._out.raw[:n]

    def close(self) -> None:
        if self._state:
            self._lib.opus_encoder_destroy(self._state)
            self._state = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
