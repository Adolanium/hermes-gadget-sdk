"""Outbound audio is paced: never more than PLAYBACK_LEAD_S ahead of real time, before or after a stall."""

from __future__ import annotations

import asyncio
from collections import Counter

import pytest

from hermes_gadget_plugin import hub as hubmod
from hermes_gadget_plugin import protocol

RATE = 16000
FRAME_BYTES = RATE * 2 * hubmod.AUDIO_FRAME_MS // 1000       # one 40 ms frame
LEAD_FRAMES = int(hubmod.PLAYBACK_LEAD_S * 1000 / hubmod.AUDIO_FRAME_MS)  # 12 frames fit in the lead


class VirtualClock:
    """The loop reads time from here, and a patched asyncio.sleep advances it instead of waiting."""

    def __init__(self):
        self.now = 1000.0


class RecordingSession:
    speaker_rate = RATE

    def __init__(self, clock: VirtualClock):
        self.clock = clock
        self.sent: list[tuple[float, int]] = []  # (virtual time, frame length)
        self.events: list[str] = []

    async def send_json(self, message: dict):
        self.events.append(message["type"])

    async def send_binary(self, data: bytes):
        self.sent.append((self.clock.now, len(data)))


async def _drain(out: hubmod.AudioOut):
    while not out._queue.empty():
        await asyncio.sleep(0)
    for _ in range(5):
        await asyncio.sleep(0)


@pytest.mark.parametrize("stall_s", [0.0, 2.0])
def test_a_reply_never_bursts_past_the_lead(monkeypatch, stall_s):
    clock = VirtualClock()
    real_sleep = asyncio.sleep

    async def fake_sleep(seconds, *args):
        if seconds <= 0:
            await real_sleep(0)
            return
        clock.now += seconds
        await real_sleep(0)

    monkeypatch.setattr(hubmod.asyncio, "sleep", fake_sleep)

    async def run():
        loop = asyncio.get_running_loop()
        monkeypatch.setattr(loop, "time", lambda: clock.now)
        session = RecordingSession(clock)
        out = hubmod.AudioOut(session, stream=7, src_rate=RATE)
        await out.start()
        out.write(b"\x00" * (FRAME_BYTES * 25))   # a one-second clause arrives at once
        await _drain(out)
        if stall_s:
            clock.now += stall_s                      # the TTS producer pauses between clauses
        out.write(b"\x00" * (FRAME_BYTES * 25))   # the next clause arrives at once
        out.finish()
        await asyncio.wait_for(out.done.wait(), 5)
        return session

    session = asyncio.run(run())
    assert session.events == ["audio.start", "audio.end"]
    assert len(session.sent) == 50 and all(n == FRAME_BYTES + protocol.BINARY_HEADER for _, n in session.sent)
    per_instant = Counter(t for t, _ in session.sent)
    biggest = max(per_instant.values())
    assert biggest <= LEAD_FRAMES + 1, f"{biggest} frames went out at one instant; the device buffer holds about 1.5 s"
    # Every frame is sent at or before the moment its audio is due, and never more than the lead early.
    # After a stall the clock restarts from the stall's end, so measure from each run's first frame.
    runs = [session.sent[:25], session.sent[25:]] if stall_s else [session.sent]
    for run_frames in runs:
        t0 = run_frames[0][0]
        for i, (t, _) in enumerate(run_frames):
            due = t0 + i * hubmod.AUDIO_FRAME_MS / 1000
            assert t >= due - hubmod.PLAYBACK_LEAD_S - 1e-6, f"frame {i} sent {due - t:.3f}s early"


@pytest.mark.parametrize("ttfa_s", [0.5, 3.0])
def test_time_to_first_audio_never_distorts_pacing(monkeypatch, ttfa_s):
    """Q3-1 (Opus sign-off): the clock starts at the first audio frame, so a long
    time-to-first-audio (TTF) — below or above the stall threshold — must not cause
    a catch-up burst at the start of the stream."""
    clock = VirtualClock()
    real_sleep = asyncio.sleep

    async def fake_sleep(seconds, *args):
        if seconds <= 0:
            await real_sleep(0)
            return
        clock.now += seconds
        await real_sleep(0)

    monkeypatch.setattr(hubmod.asyncio, "sleep", fake_sleep)

    async def run():
        loop = asyncio.get_running_loop()
        monkeypatch.setattr(loop, "time", lambda: clock.now)
        session = RecordingSession(clock)
        out = hubmod.AudioOut(session, stream=7, src_rate=RATE)
        await out.start()
        clock.now += ttfa_s                # the TTS producer takes ttfa_s to emit the first audio
        out.write(b"\x00" * (FRAME_BYTES * 25))   # a one-second clause arrives at once
        out.finish()
        await asyncio.wait_for(out.done.wait(), 5)
        return session

    session = asyncio.run(run())
    assert session.events == ["audio.start", "audio.end"]
    assert len(session.sent) == 25 and all(n == FRAME_BYTES + protocol.BINARY_HEADER for _, n in session.sent)
    per_instant = Counter(t for t, _ in session.sent)
    biggest = max(per_instant.values())
    assert biggest <= LEAD_FRAMES + 1, f"{biggest} frames burst at one instant after a {ttfa_s}s time-to-first-audio"
    # All 25 frames go out paced from the first frame's send time, regardless of the TTF delay.
    t0 = session.sent[0][0]
    for i, (t, _) in enumerate(session.sent):
        due = t0 + i * hubmod.AUDIO_FRAME_MS / 1000
        assert t >= due - hubmod.PLAYBACK_LEAD_S - 1e-6, f"frame {i} sent {due - t:.3f}s early"
