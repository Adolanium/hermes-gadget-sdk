"""PortAudio streams for a real device. No recordings or silence fallback."""

from __future__ import annotations

import math
import threading
import time
from array import array


def backend():
    try:
        import sounddevice
    except (ImportError, OSError) as exc:
        raise RuntimeError("install the audio extra and libportaudio2 to use audio") from exc
    return sounddevice


def check(config: dict) -> dict:
    sd = backend()
    rate = config.get("rate", 16000)
    result = {"rate": rate}
    for kind in ("input", "output"):
        if kind in config:
            try:
                getattr(sd, f"check_{kind}_settings")(device=config[kind], channels=1, dtype="int16", samplerate=rate)
                result[kind] = sd.query_devices(config[kind], kind)["name"]
            except sd.PortAudioError as exc:
                raise RuntimeError(f"audio {kind} is unavailable: {exc}") from exc
    return result


def check_loopback(config: dict) -> None:
    """Explicit local microphone/speaker check; the samples never reach Hermes."""
    if "input" not in config or "output" not in config:
        raise ValueError("audio-check needs both audio.input and audio.output")
    devices = check(config)
    sd = backend()
    rate = devices["rate"]
    print(f"Speak for three seconds into {devices['input']}.", flush=True)
    with sd.RawInputStream(device=config["input"], samplerate=rate, channels=1, dtype="int16") as mic:
        data, overflow = mic.read(rate * 3)
    samples = array("h")
    samples.frombytes(data)
    peak = max((abs(value) for value in samples), default=0)
    db = 20 * math.log10(peak / 32768) if peak else -96
    print(f"Peak: {db:.1f} dBFS. Playing back at half volume on {devices['output']}.", flush=True)
    if overflow:
        raise RuntimeError("input overflow; try another audio device or sample rate")
    pcm = array("h", (int(value / 2) for value in samples)).tobytes()
    with sd.RawOutputStream(device=config["output"], samplerate=rate, channels=1, dtype="int16") as speaker:
        if speaker.write(pcm):
            raise RuntimeError("output underflow; try another audio device or sample rate")
    if peak == 0:
        raise RuntimeError("no microphone signal was captured; check the selected device and ALSA capture level")


class Audio:
    def __init__(self, config: dict):
        self.config = config
        self.devices = check(config)
        self.mic = None
        self.speaker = None
        self.errors: dict[str, str] = {}
        self.volume = 70
        self.rate = config.get("rate", 16000)
        self.input_buffer = bytearray()
        self.output_buffer = bytearray()
        self.lock = threading.Lock()
        self.ended = True
        self.play_until = 0.0

    def mic_start(self, rate: int) -> bool:
        self.mic_stop()
        self.errors.pop("input", None)
        sd = backend()

        def capture(data, frames, timing, status):
            with self.lock:
                if status or len(self.input_buffer) + len(data) > rate * 4:
                    self.errors["input"] = str(status) if status else "microphone buffer overflow"
                    raise sd.CallbackAbort
                self.input_buffer.extend(data)

        try:
            self.mic = sd.RawInputStream(device=self.config["input"], samplerate=rate, channels=1,
                                         dtype="int16", callback=capture)
            self.mic.start()
            return True
        except (sd.PortAudioError, OSError, ValueError) as exc:
            self.errors["input"] = str(exc)
            self.mic_stop()
            return False

    def mic_stop(self) -> None:
        if self.mic is not None:
            try:
                self.mic.abort()
            finally:
                self.mic.close()
                self.mic = None
        with self.lock:
            self.input_buffer.clear()

    def read(self) -> bytes:
        if self.mic is not None and not self.mic.active:
            self.errors.setdefault("input", "microphone stream stopped")
        with self.lock:
            data = bytes(self.input_buffer)
            self.input_buffer.clear()
            return data

    def speaker_begin(self, rate: int) -> bool:
        self.speaker_abort()
        self.errors.pop("output", None)
        self.rate = rate
        self.ended = False
        sd = backend()

        def playback(data, frames, timing, status):
            with self.lock:
                count = min(len(data), len(self.output_buffer))
                data[:count] = self.output_buffer[:count]
                del self.output_buffer[:count]
                data[count:] = bytes(len(data) - count)
                if count:
                    # Include device latency so busy() covers the final audible samples.
                    self.play_until = time.monotonic() + max(0, timing.outputBufferDacTime - timing.currentTime) + count / 2 / rate
                if status:
                    self.errors["output"] = str(status)

        try:
            self.speaker = sd.RawOutputStream(device=self.config["output"], samplerate=rate, channels=1,
                                              dtype="int16", callback=playback)
            self.speaker.start()
            return True
        except (sd.PortAudioError, OSError, ValueError) as exc:
            self.errors["output"] = str(exc)
            self.speaker_abort()
            return False

    def speaker_write(self, pcm: bytes) -> None:
        if self.speaker is None:
            return
        samples = array("h")
        samples.frombytes(pcm)
        scaled = array("h", (int(value * self.volume / 100) for value in samples)).tobytes()
        with self.lock:
            overflow = len(self.output_buffer) + len(scaled) > self.rate * 2 * 30
            if not overflow:
                self.output_buffer.extend(scaled)
        if overflow:
            self.errors["output"] = "speaker buffer exceeded 30 seconds"
            self.speaker_abort()

    def speaker_end(self) -> None:
        self.ended = True

    def speaker_busy(self) -> bool:
        if self.speaker is None:
            return False
        if not self.speaker.active:
            self.errors.setdefault("output", "speaker stream stopped")
            self.speaker_abort()
            return False
        with self.lock:
            busy = not self.ended or bool(self.output_buffer) or time.monotonic() < self.play_until
        if not busy:
            self.speaker_abort()
        return busy

    def speaker_abort(self) -> None:
        if self.speaker is not None:
            try:
                self.speaker.abort()
            finally:
                self.speaker.close()
                self.speaker = None
        with self.lock:
            self.output_buffer.clear()
            self.ended = True
            self.play_until = 0.0

    def speaker_volume(self, percent: int) -> None:
        self.volume = percent

    def close(self) -> None:
        try:
            self.mic_stop()
        finally:
            self.speaker_abort()
