"""PCM helpers: WAV packing, streaming resampling and file decoding.

Devices speak mono PCM16 at a rate they declare. Hermes's TTS produces files
(MP3/OGG/WAV, provider dependent) or streaming PCM at the provider's rate, so
everything is normalised here before it reaches a device. Pure Python (no
``audioop``, removed in 3.13); ffmpeg is used for compressed formats when
present.
"""

from __future__ import annotations

import io
import shutil
import subprocess
import sys
import wave
from array import array


class AudioDecodeError(RuntimeError):
    pass


def wav_bytes(pcm: bytes, rate: int, channels: int = 1) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


def _samples(pcm: bytes) -> array:
    a = array("h")
    a.frombytes(pcm[: len(pcm) - (len(pcm) % 2)])
    if sys.byteorder == "big":
        a.byteswap()
    return a


def _to_bytes(a: array) -> bytes:
    if sys.byteorder == "big":
        a = array("h", a)
        a.byteswap()
    return a.tobytes()


def downmix(samples: array, channels: int) -> array:
    if channels <= 1:
        return samples
    out = array("h")
    for i in range(0, len(samples) - channels + 1, channels):
        out.append(sum(samples[i : i + channels]) // channels)
    return out


class Resampler:
    """Streaming linear-interpolation resampler for mono/interleaved PCM16.

    Keeps the fractional read position and the last input sample between
    chunks, so a stream cut into arbitrary chunk sizes resamples exactly like
    one contiguous buffer.
    """

    def __init__(self, src_rate: int, dst_rate: int, channels: int = 1):
        if src_rate <= 0 or dst_rate <= 0:
            raise ValueError("sample rates must be positive")
        self.src_rate, self.dst_rate, self.channels = src_rate, dst_rate, max(1, channels)
        self._step = src_rate / dst_rate
        self._pos = 0.0
        self._prev: int | None = None
        self._odd = b""

    def process(self, pcm: bytes) -> bytes:
        data = self._odd + pcm
        frame_bytes = 2 * self.channels
        cut = len(data) - (len(data) % frame_bytes)
        self._odd = data[cut:]
        mono = downmix(_samples(data[:cut]), self.channels)
        if self.src_rate == self.dst_rate:
            return _to_bytes(mono)
        buf = mono if self._prev is None else array("h", [self._prev]) + mono
        n = len(buf)
        if n < 2:
            if n == 1:
                self._prev = buf[0]
            return b""
        out = array("h")
        pos, step = self._pos, self._step
        last = n - 1
        while pos < last:
            i = int(pos)
            a = buf[i]
            out.append(int(a + (buf[i + 1] - a) * (pos - i)))
            pos += step
        # The next chunk is prefixed with this chunk's last sample (index 0).
        self._pos = pos - last
        self._prev = buf[last]
        return _to_bytes(out)


def resample(pcm: bytes, src_rate: int, dst_rate: int, channels: int = 1) -> bytes:
    return Resampler(src_rate, dst_rate, channels).process(pcm)


def find_ffmpeg() -> str | None:
    return shutil.which("ffmpeg")


def decode_file(path: str, rate: int) -> bytes:
    """Decode an audio file to mono PCM16 at ``rate``."""
    try:
        with wave.open(path, "rb") as w:
            if w.getsampwidth() == 2 and w.getcomptype() == "NONE":
                raw = w.readframes(w.getnframes())
                return resample(raw, w.getframerate(), rate, w.getnchannels())
    except (wave.Error, EOFError):
        pass  # not a plain PCM WAV; fall through to ffmpeg
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        raise AudioDecodeError(f"cannot decode {path}: not PCM WAV and ffmpeg is not installed")
    proc = subprocess.run(
        [ffmpeg, "-nostdin", "-v", "error", "-i", path, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1",
         "-ar", str(rate), "-"],
        capture_output=True,
        timeout=120,
    )
    if proc.returncode != 0:
        raise AudioDecodeError(f"ffmpeg failed: {proc.stderr.decode(errors='replace').strip()[:200]}")
    return proc.stdout


def duration_s(pcm: bytes, rate: int) -> float:
    return len(pcm) / 2 / rate if rate else 0.0
