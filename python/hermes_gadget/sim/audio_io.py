"""Simulated microphone and speaker.

Microphone input comes from WAV files (scripted, deterministic) or the PC
microphone (``sounddevice``). Speaker output is always recorded to WAV files
and optionally played on the PC speakers. Playback timing is simulated from
the sample count, so the device UI shows "Speaking" for as long as real
hardware would.
"""

from __future__ import annotations

import logging
import threading
import time
import wave
from collections import deque
from pathlib import Path

from hermes_gadget_plugin import audio as pcm_tools

log = logging.getLogger("hermes_gadget.sim.audio")


def sounddevice_available() -> bool:
    try:
        import sounddevice  # noqa: F401
    except Exception:  # ImportError, or OSError when PortAudio is missing
        return False
    return True


def load_wav(path: str | Path, rate: int) -> bytes:
    return pcm_tools.decode_file(str(path), rate)


class Microphone:
    """Pull-based microphone: ``read()`` returns the PCM captured since the last call."""

    def __init__(self, live: bool = False):
        self.rate = 16000
        self.active = False
        self._queue: deque[bytes] = deque()
        self._last = 0.0
        self._live_wanted = live and sounddevice_available()
        self._stream = None
        self._live_buf = bytearray()
        self._lock = threading.Lock()
        self.level = 0

    @property
    def live(self) -> bool:
        return self._live_wanted

    def inject(self, pcm: bytes) -> None:
        """Queue audio (at the active rate) to be "spoken" into the mic."""
        self._queue.append(pcm)

    def queued_seconds(self) -> float:
        return sum(len(c) for c in self._queue) / 2 / self.rate if self.rate else 0.0

    def start(self, rate: int) -> bool:
        self.rate = rate
        self.active = True
        self._last = time.monotonic()
        if self._live_wanted and not self._queue:
            try:
                import sounddevice as sd

                def callback(indata, frames, time_info, status):
                    with self._lock:
                        self._live_buf += bytes(indata)

                self._stream = sd.RawInputStream(samplerate=rate, channels=1, dtype="int16", callback=callback)
                self._stream.start()
            except Exception as exc:
                log.warning("live microphone unavailable (%s); using silence", exc)
                self._stream = None
        return True

    def stop(self) -> None:
        self.active = False
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            finally:
                self._stream = None
        with self._lock:
            self._live_buf.clear()

    def read(self) -> bytes:
        if not self.active:
            return b""
        if self._stream is not None:
            with self._lock:
                data = bytes(self._live_buf)
                self._live_buf.clear()
            return data
        now = time.monotonic()
        want = int((now - self._last) * self.rate) * 2
        if want < 640:  # wait for at least 20 ms worth
            return b""
        self._last = now
        out = bytearray()
        while len(out) < want and self._queue:
            chunk = self._queue[0]
            take = want - len(out)
            out += chunk[:take]
            if take >= len(chunk):
                self._queue.popleft()
            else:
                self._queue[0] = chunk[take:]
        out += bytes(want - len(out))  # silence once the injected audio runs out
        return bytes(out)


class Speaker:
    def __init__(self, out_dir: Path | None, live: bool = False):
        self.out_dir = out_dir
        self.volume = 70
        self.rate = 16000
        self._open = False
        self._until = 0.0
        self._pcm = bytearray()
        self._count = 0
        self.last_file: Path | None = None
        self._live = live and sounddevice_available()
        self._stream = None
        self._play_buf = bytearray()
        self._lock = threading.Lock()

    def begin(self, rate: int) -> bool:
        self.abort()
        self.rate = rate
        self._open = True
        self._until = time.monotonic()
        self._pcm.clear()
        if self._live:
            try:
                import sounddevice as sd

                def callback(outdata, frames, time_info, status):
                    need = len(outdata)
                    with self._lock:
                        chunk = bytes(self._play_buf[:need])
                        del self._play_buf[:need]
                    outdata[: len(chunk)] = chunk
                    if len(chunk) < need:
                        outdata[len(chunk):] = bytes(need - len(chunk))

                self._stream = sd.RawOutputStream(samplerate=rate, channels=1, dtype="int16", callback=callback)
                self._stream.start()
            except Exception as exc:
                log.warning("live speaker unavailable (%s); recording only", exc)
                self._stream = None
        return True

    def write(self, pcm: bytes) -> None:
        if not self._open:
            return
        self._pcm += pcm
        now = time.monotonic()
        self._until = max(self._until, now) + len(pcm) / 2 / self.rate
        if self._stream is not None:
            scaled = pcm if self.volume >= 100 else _scale(pcm, self.volume)
            with self._lock:
                self._play_buf += scaled

    def end(self) -> None:
        if not self._open:
            return
        self._open = False
        self._save()

    def abort(self) -> None:
        if self._open:
            self._save()
        self._open = False
        self._until = 0.0
        with self._lock:
            self._play_buf.clear()
        self._close_stream()

    def busy(self) -> bool:
        playing = self._open or time.monotonic() < self._until
        if not playing:
            self._close_stream()
        return playing

    def _close_stream(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:
                pass
            self._stream = None

    def _save(self) -> None:
        if not self.out_dir or not self._pcm:
            return
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self._count += 1
        path = self.out_dir / f"speaker-{time.strftime('%H%M%S')}-{self._count:03d}.wav"
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(self.rate)
            w.writeframes(bytes(self._pcm))
        self.last_file = path
        self._pcm.clear()


def _scale(pcm: bytes, percent: int) -> bytes:
    from array import array

    a = array("h")
    a.frombytes(pcm[: len(pcm) - len(pcm) % 2])
    f = max(0, min(100, percent)) / 100.0
    return array("h", (int(s * f) for s in a)).tobytes()
