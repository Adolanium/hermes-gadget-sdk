"""Opus speaker audio: negotiated from the device's formats, encoded per frame, paced by audio time."""

from __future__ import annotations

import asyncio
import ctypes
import math
import os
import struct
from types import SimpleNamespace

import pytest

from hermes_gadget_plugin import hub as hubmod
from hermes_gadget_plugin import opus, protocol

RATE = 16000
FRAME_BYTES = RATE * 2 * hubmod.AUDIO_FRAME_MS // 1000

# CI sets HG_REQUIRE_LIBOPUS=1 after installing libopus, so a missing library fails there instead of skipping.
needs_libopus = pytest.mark.skipif(
    not opus.available() and not os.environ.get("HG_REQUIRE_LIBOPUS"), reason="libopus is not installed")


@pytest.mark.skipif(not os.environ.get("HG_REQUIRE_LIBOPUS"), reason="libopus not required here")
def test_libopus_present_when_required():
    assert opus.available(), "HG_REQUIRE_LIBOPUS is set but libopus did not load"


class Session:
    speaker_rate = RATE

    def __init__(self, formats, bitrate=24000):
        self.caps = {"speaker": {"rate": RATE, "format": "pcm16", "formats": formats}} if formats else {
            "speaker": {"rate": RATE, "format": "pcm16"}}
        self.hub = SimpleNamespace(opus_bitrate=bitrate)
        self.device_id = "hg-test"
        self.messages: list[dict] = []
        self.frames: list[bytes] = []

    speaker_formats = hubmod.DeviceSession.speaker_formats

    async def send_json(self, message: dict):
        self.messages.append(message)

    async def send_binary(self, data: bytes):
        self.frames.append(data)


def _tone(frames: int) -> bytes:
    n = frames * FRAME_BYTES // 2
    return b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * 300 * i / RATE))) for i in range(n))


async def _play(session, pcm: bytes):
    out = hubmod.AudioOut(session, stream=3, src_rate=RATE)
    await out.start()
    out.write(pcm)
    out.finish()
    await asyncio.wait_for(out.done.wait(), 10)
    return out


def test_speaker_formats_always_include_pcm16():
    assert Session(None).speaker_formats == ["pcm16"]
    assert Session(["pcm16", "opus"]).speaker_formats == ["pcm16", "opus"]
    assert Session(["opus"]).speaker_formats == ["pcm16", "opus"]
    assert Session("opus").speaker_formats == ["pcm16"]  # malformed: ignored


@pytest.mark.parametrize("formats, bitrate", [(None, 24000), (["pcm16"], 24000), (["pcm16", "opus"], 0)])
def test_pcm16_unless_the_device_decodes_opus_and_it_is_enabled(formats, bitrate):
    session = Session(formats, bitrate)
    out = asyncio.run(_play(session, _tone(3)))
    assert out.format == "pcm16"
    assert session.messages[0]["format"] == "pcm16"
    assert all(len(f) == protocol.BINARY_HEADER + FRAME_BYTES for f in session.frames)


@needs_libopus
def test_opus_frames_are_small_paced_by_audio_time_and_decode_back():
    session = Session(["pcm16", "opus"])
    pcm = _tone(25) + _tone(1)[: FRAME_BYTES // 2]  # one second plus a partial frame
    out = asyncio.run(_play(session, pcm))
    assert out.format == "opus" and session.messages[0]["format"] == "opus"
    assert session.messages[-1]["type"] == "audio.end"
    assert len(session.frames) == 26  # one packet per frame, the partial one padded
    payloads = [f[protocol.BINARY_HEADER:] for f in session.frames]
    assert sum(map(len, payloads)) < len(pcm) / 6  # 24 kbps against 256 kbps of PCM16
    # Pacing counts the audio sent, not the bytes on the wire.
    assert out.sent_seconds == pytest.approx(len(pcm) / 2 / RATE)

    # Decode with libopus, as the device does, and compare with the source tone.
    lib = opus._load()
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
                                ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    err = ctypes.c_int(0)
    dec = lib.opus_decoder_create(RATE, 1, ctypes.byref(err))
    assert err.value == 0
    buf = (ctypes.c_int16 * 1920)()
    decoded: list[int] = []
    for p in payloads:
        n = lib.opus_decode(dec, p, len(p), buf, 1920, 0)
        assert n == FRAME_BYTES // 2
        decoded.extend(buf[:n])
    lib.opus_decoder_destroy(ctypes.c_void_p(dec))
    src = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    # Skip the codec's start-up delay, then the tone must come through closely.
    lag = max(range(0, 400), key=lambda d: sum(src[i] * decoded[i + d] for i in range(2000, 6000)))
    a, b = src[2000:12000], decoded[2000 + lag:12000 + lag]
    corr = sum(x * y for x, y in zip(a, b)) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b))
    assert corr > 0.9
