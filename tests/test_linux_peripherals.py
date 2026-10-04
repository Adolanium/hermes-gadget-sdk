"""Audio frames and GPIO behavior with the hardware boundary replaced."""

import asyncio
import json
import struct
from types import SimpleNamespace

import pytest
from conftest import requires_sim
from gpiozero.pins.mock import MockFactory
from hermes_gadget.linux import audio, gpio
from hermes_gadget.linux.client import Client, load_config
from test_linux import wait


class Stream:
    def __init__(self, *, callback, **kwargs):
        self.callback = callback
        self.active = False
        self.closed = False
        self.settings = kwargs

    def start(self):
        self.active = True

    def abort(self):
        self.active = False

    def close(self):
        self.closed = True


@pytest.fixture
def audio_backend(monkeypatch):
    backend = SimpleNamespace(
        RawInputStream=Stream, RawOutputStream=Stream, PortAudioError=RuntimeError,
        check_input_settings=lambda **_: None, check_output_settings=lambda **_: None,
        query_devices=lambda device, kind: {"name": f"USB {kind}"}, CallbackAbort=RuntimeError)
    monkeypatch.setattr(audio, "backend", lambda: backend)
    return backend


def test_audio_capture_volume_drain_cancel_and_failure(audio_backend):
    device = audio.Audio({"input": 1, "output": 2})
    try:
        assert device.mic_start(16000) is True
        device.mic.callback(b"\x10\x00\x20\x00", 2, None, None)
        assert device.read() == b"\x10\x00\x20\x00"
        device.mic_stop()
        assert device.read() == b""
        assert device.speaker_begin(16000) is True
        device.speaker_volume(50)
        device.speaker_write(struct.pack("<hh", 1000, -1000))
        output = bytearray(8)
        device.speaker.callback(output, 4, SimpleNamespace(outputBufferDacTime=0, currentTime=0), None)
        assert output == struct.pack("<hhhh", 500, -500, 0, 0)
        assert device.speaker_busy() is True
        device.speaker_end()
        device.play_until = 0
        stream = device.speaker
        assert device.speaker_busy() is False
        assert stream.closed is True
        assert device.speaker_begin(16000) is True
        device.speaker_write(b"\x30\x00" * 20)
        device.speaker_abort()
        assert device.speaker_busy() is False
        assert device.output_buffer == b""

        def missing(**kwargs):
            raise RuntimeError("USB device disconnected")

        audio_backend.RawInputStream = missing
        assert device.mic_start(16000) is False
        assert device.errors == {"input": "USB device disconnected"}
        assert device.read() == b""
    finally:
        device.close()


def test_microphone_overflow_is_reported(audio_backend):
    device = audio.Audio({"input": 1})
    try:
        assert device.mic_start(16000) is True
        with pytest.raises(RuntimeError):
            device.mic.callback(bytes(64002), 32001, None, None)
        assert device.errors == {"input": "microphone buffer overflow"}
    finally:
        device.close()


@requires_sim
def test_gpio_actions_and_buttons_use_the_production_core(devserver, loop_thread, tmp_path, monkeypatch, audio_backend):
    factory = MockFactory()
    original = gpio.Gpio
    monkeypatch.setattr(gpio, "Gpio", lambda config, device: original(config, device, pin_factory=factory))
    hub, _, url = devserver()
    client = Client({"server": url, "gpio": {"talk": 17, "cancel": 27, "status_led": 22,
                                             "outputs": {"light": 23}}, "audio": {"input": 1}}, tmp_path)
    try:
        client.start()
        wait(client, lambda: client.device.status()["paired"])
        client.step()
        assert factory.pin(22).state is True
        session = next(iter(hub.sessions.values()))
        assert session.action_names() == ["gpio.light"]
        action = asyncio.run_coroutine_threadsafe(session.invoke_action("gpio.light", {"on": True}), loop_thread.loop)
        wait(client, action.done)
        assert action.result() == {"on": True}
        assert factory.pin(23).state is True
        factory.pin(17).drive_low()
        wait(client, lambda: client.device.screen() == "listening")
        assert client.audio.mic.active is True
        factory.pin(27).drive_low()
        wait(client, lambda: client.device.screen() != "listening")
        assert client.audio.mic is None
        assert client.device.status()["phase"] == "online"
    finally:
        client.close()
    assert factory.pin(23).state is False
    factory.close()


@pytest.mark.parametrize("field", [
    {"audio": {"input": True}}, {"audio": {"rate": 1}},
    {"gpio": {"talk": 17, "cancel": 17}}, {"gpio": {"outputs": {"lamp": 40}}},
    {"gpio": {"outputs": {"bad.name": 23}}}, {"gpio": {"chip": -1}},
])
def test_invalid_peripheral_config_is_rejected(tmp_path, field):
    path = tmp_path / "config.json"
    path.write_text(json.dumps({"server": "ws://localhost/gadget", **field}))
    with pytest.raises(ValueError):
        load_config(path)
