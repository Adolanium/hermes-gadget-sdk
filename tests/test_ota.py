"""Firmware updates on the host side: image checks, the sender, the staging queue."""

from __future__ import annotations

import asyncio
import hashlib
import types

import pytest

from fakes.fake_firmware import fake_image
from hermes_gadget_plugin import ota, protocol

KEY = bytes(range(32))
DEVICE_ID = protocol.device_id_for_key(KEY)


def test_an_image_says_what_it_is():
    image = ota.inspect_image(fake_image(board="esp32s3-touch-amoled-1.75", version="0.3.1"))
    assert (image.version, image.board, image.size) == ("0.3.1", "esp32s3-touch-amoled-1.75", 150_000)
    assert image.sha256 == hashlib.sha256(image.data).hexdigest()
    assert ota.inspect_image(fake_image(board=None)).board is None  # firmware older than the board tag


@pytest.mark.parametrize("data, code", [
    (b"not firmware" * 100, "not_an_image"),
    (b"\xe9" + bytes(400), "not_an_image"),
    (fake_image(project="other_project"), "wrong_firmware"),
], ids=["text", "no-app-description", "another-project"])
def test_other_files_are_refused(data, code):
    with pytest.raises(ota.UpdateError) as err:
        ota.inspect_image(data)
    assert err.value.code == code


def _session(**caps):
    return types.SimpleNamespace(name="Desk", board="sim-320x240", caps=caps)


def test_the_image_must_suit_the_device():
    image = ota.inspect_image(fake_image())
    ota.check_for(image, _session(ota={"max_size": 200_000}))
    for session, code in [
        (_session(), "unsupported"),
        (_session(ota={"max_size": 100_000}), "too_large"),
        (types.SimpleNamespace(name="Desk", board="esp32s3-breadboard", caps={"ota": {}}), "wrong_board"),
    ]:
        with pytest.raises(ota.UpdateError) as err:
            ota.check_for(image, session)
        assert err.value.code == code


class FakeDevice:
    """Plays the firmware's side of an update (firmware/core/src/app.cpp), over a fake session."""

    def __init__(self, *, fail_at: int | None = None, silent: bool = False):
        self.name, self.device_id, self.closed = "Desk", DEVICE_ID, False
        self._ota_inbox = None
        self.fail_at, self.silent = fail_at, silent
        self.offer: dict = {}
        self.nonce = "bm9uY2U="
        self.received = bytearray()
        self.acked = 0
        self.sent_types: list[str] = []
        self.max_in_flight = 0

    def _next_stream(self) -> int:
        return 5

    def _reply(self, type_: str, **fields) -> None:
        if not self.silent:
            self._ota_inbox.put_nowait(protocol.message(type_, **fields))

    async def send_json(self, msg: dict) -> None:
        self.sent_types.append(msg["type"])
        if msg["type"] == "ota.offer":
            self.offer = msg
            self._reply("ota.ready", nonce=self.nonce)
        elif msg["type"] == "ota.begin":
            want = protocol.ota_mac(KEY, DEVICE_ID, self.nonce, self.offer["sha256"], self.offer["size"])
            if msg["mac"] != want:
                self._reply("ota.error", code="unauthorized", message="wrong key")
            else:
                self._reply("ota.ack", offset=0)
        elif msg["type"] == "ota.end":
            ok = hashlib.sha256(self.received).hexdigest() == self.offer["sha256"]
            self._reply("ota.done", version=self.offer["version"]) if ok else self._reply(
                "ota.error", code="checksum", message="bad image")

    async def send_binary(self, data: bytes) -> None:
        frame = protocol.parse_binary(data)
        assert frame.channel == protocol.CHANNEL_FIRMWARE and frame.stream == 5
        self.received += frame.payload
        self.max_in_flight = max(self.max_in_flight, len(self.received) - self.acked)
        if self.fail_at is not None and len(self.received) >= self.fail_at:
            self._reply("ota.error", code="flash", message="writing to flash failed")
            self.fail_at = None
            return
        if len(self.received) - self.acked >= 16 * 1024 or len(self.received) == self.offer["size"]:
            self.acked = len(self.received)
            self._reply("ota.ack", offset=self.acked)


def test_an_update_streams_within_the_window_and_reports_progress():
    image = ota.inspect_image(fake_image(size=300_000))
    device, seen = FakeDevice(), []
    version = asyncio.run(ota.send_update(device, image, KEY, progress=lambda sent, total: seen.append(sent)))
    assert version == "0.2.0"
    assert bytes(device.received) == image.data
    assert device.max_in_flight <= ota.WINDOW_BYTES
    assert seen[-1] == image.size and seen == sorted(seen)
    assert device._ota_inbox is None  # the next update may start


def test_device_refusals_surface_with_their_reason():
    image = ota.inspect_image(fake_image())
    with pytest.raises(ota.UpdateError) as err:
        asyncio.run(ota.send_update(FakeDevice(), image, bytes(32)))
    assert err.value.code == "unauthorized"

    device = FakeDevice(fail_at=40_000)
    with pytest.raises(ota.UpdateError) as err:
        asyncio.run(ota.send_update(device, image, KEY))
    assert (err.value.code, err.value.message) == ("flash", "writing to flash failed")
    assert "ota.end" not in device.sent_types


def test_a_silent_device_times_out_and_the_update_is_withdrawn(monkeypatch):
    monkeypatch.setattr(ota, "REPLY_TIMEOUT_S", 0.05)
    device = FakeDevice(silent=True)
    with pytest.raises(ota.UpdateError) as err:
        asyncio.run(ota.send_update(device, ota.inspect_image(fake_image()), KEY))
    assert err.value.code == "timeout"
    assert device.sent_types == ["ota.offer", "ota.abort"]


def test_the_queue_hands_staged_images_to_the_gateway(tmp_path):
    queue = ota.UpdateQueue(tmp_path)
    image = ota.inspect_image(fake_image())
    assert queue.pending() == []
    queue.stage(DEVICE_ID, image)
    assert queue.pending() == [DEVICE_ID]
    data, meta = queue.load(DEVICE_ID)
    assert data == image.data and meta["version"] == "0.2.0" and meta["attempts"] == 0
    assert [queue.count_attempt(DEVICE_ID) for _ in range(2)] == [1, 2]
    queue.report(DEVICE_ID, state="sending", sent=10, size=20)
    assert queue.status(DEVICE_ID)["state"] == "sending"
    queue.drop(DEVICE_ID)
    assert queue.pending() == [] and queue.load(DEVICE_ID) is None
    assert queue.status(DEVICE_ID)["state"] == "sending"  # the last word stays for the CLI
    queue.stage(DEVICE_ID, image)
    assert queue.status(DEVICE_ID) is None  # a new update starts with no status
    with pytest.raises(ValueError):
        queue.stage("../evil", image)


@pytest.fixture
def cli_store(tmp_path, monkeypatch):
    from hermes_gadget_plugin import cli
    from hermes_gadget_plugin.store import DeviceStore

    store = DeviceStore(tmp_path)
    monkeypatch.setattr(cli, "_store", lambda: store)
    firmware = tmp_path / "firmware.bin"
    firmware.write_bytes(fake_image(board="sim-320x240", version="0.2.0"))
    return store, firmware


def test_hermes_gadget_update_stages_and_waits_for_the_gateway(cli_store, monkeypatch, capsys):
    from hermes_gadget_plugin import cli

    store, firmware = cli_store
    store.enroll(DEVICE_ID, KEY, name="Desk", board="sim-320x240")
    queue = ota.UpdateQueue(store.path.parent)

    cli._cmd_update(types.SimpleNamespace(latest=False, force=False, device="desk", image=str(firmware), no_wait=True, timeout=1))
    assert queue.pending() == [DEVICE_ID]
    assert "Staged firmware 0.2.0" in capsys.readouterr().out

    reports = iter([dict(state="sending", sent=50, size=100), dict(state="done", version="0.2.0")])
    monkeypatch.setattr(cli.time, "sleep", lambda s: queue.report(DEVICE_ID, **next(reports)))
    cli._cmd_update(types.SimpleNamespace(latest=False, force=False, device=DEVICE_ID, image=str(firmware), no_wait=False, timeout=30))
    out = capsys.readouterr().out
    assert "sending: 50%" in out and "Installed 0.2.0. Desk restarts into it" in out


def test_hermes_gadget_update_refuses_another_boards_image(cli_store):
    from hermes_gadget_plugin import cli

    store, firmware = cli_store
    store.enroll(DEVICE_ID, KEY, name="Desk", board="esp32s3-breadboard")
    with pytest.raises(SystemExit) as stop:
        cli._cmd_update(types.SimpleNamespace(latest=False, force=False, device="Desk", image=str(firmware), no_wait=True, timeout=1))
    assert "is built for sim-320x240, but Desk is esp32s3-breadboard" in str(stop.value)
    assert ota.UpdateQueue(store.path.parent).pending() == []
