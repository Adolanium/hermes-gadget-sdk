"""A recording is assembled by stream, however its frames are paced: a device that held one
through a lost connection sends it whole, at once, on the new connection."""

from __future__ import annotations

import asyncio
import logging

from hermes_gadget_plugin import hub as hubmod
from hermes_gadget_plugin import protocol

RATE = 16000
FRAME_SAMPLES = 640


class FakeSession:
    device_id = "hg-test"

    def __init__(self):
        self._upload = None
        self.spawned: list = []
        self.notices: list[str] = []

    def spawn(self, coro):
        self.spawned.append(asyncio.ensure_future(coro))

    async def send_notice(self, text):
        self.notices.append(text)

    async def turn_end(self, turn, outcome="success"):
        pass


class Delegate:
    def __init__(self):
        self.utterances: list[tuple[str, bytes, float]] = []

    async def on_utterance(self, session, msg_id, wav, seconds):
        self.utterances.append((msg_id, wav, seconds))


def _upload(hub, session, msg_id, stream, seconds, **start_extra):
    async def run():
        await hub._h_audio_start(session, protocol.message(
            "audio.start", id=msg_id, stream=stream, rate=RATE, format="pcm16", mode="hold", **start_extra))
        samples = int(RATE * seconds)
        seq = 0
        for off in range(0, samples, FRAME_SAMPLES):
            n = min(FRAME_SAMPLES, samples - off)
            hub._on_binary(session, protocol.binary(protocol.CHANNEL_AUDIO, stream, seq, b"\x01\x00" * n))
            seq += 1
        await hub._h_audio_end(session, protocol.message("audio.end", id=msg_id, stream=stream,
                                                          duration_ms=int(seconds * 1000)))
        await asyncio.gather(*session.spawned)
    asyncio.run(run())


def test_a_resent_recording_arriving_all_at_once_is_one_whole_utterance(caplog):
    delegate = Delegate()
    hub = hubmod.DeviceHub(None, delegate)
    session = FakeSession()
    with caplog.at_level(logging.INFO, logger=hubmod.log.name):
        _upload(hub, session, "a7", stream=9, seconds=30.0, resent=1, age_ms=41000)
    assert len(delegate.utterances) == 1
    msg_id, wav, seconds = delegate.utterances[0]
    assert msg_id == "a7"  # the request the device started before the connection dropped
    assert abs(seconds - 30.0) < 0.01  # all of it: nothing dropped for arriving faster than real time
    assert len(wav) == 44 + RATE * 2 * 30
    assert not session.notices
    assert "sent again after a lost connection" in caplog.text


def test_audio_start_without_the_resend_fields_is_unchanged():
    delegate = Delegate()
    hub = hubmod.DeviceHub(None, delegate)
    _upload(hub, FakeSession(), "a8", stream=3, seconds=1.0)
    assert [(m, round(s, 2)) for m, _w, s in delegate.utterances] == [("a8", 1.0)]
