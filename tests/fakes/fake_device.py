"""A connected device as the hub sees it: a real ``DeviceSession`` over a socket that records.

Everything the session sends is kept in order, so a test can read back the audio stream a
device would have received without a simulator or a network.
"""

from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace

from hermes_gadget_plugin import hub as hubmod
from hermes_gadget_plugin import protocol


class RecordingSocket:
    """``delay`` makes each send take that long, as a slow link does: the sender is suspended."""

    def __init__(self, delay: float = 0.0):
        self.sent: list = []  # dicts for JSON messages, protocol.BinaryFrame for binary ones
        self.delay = delay

    async def send(self, payload) -> None:
        if self.delay:
            await asyncio.sleep(self.delay)
        self.sent.append(protocol.parse_binary(payload) if isinstance(payload, bytes) else json.loads(payload))


class FakeDevice(hubmod.DeviceSession):
    """A paired device with a speaker at ``rate``. ``messages`` and ``audio`` read what it was sent."""

    def __init__(self, rate: int = 16000, speaker: bool = True, send_delay: float = 0.0):
        caps = {"speaker": {"rate": rate, "format": "pcm16"}} if speaker else {}
        super().__init__(SimpleNamespace(opus_bitrate=0, _tasks=set(), _held={}), RecordingSocket(send_delay),
                         {"device_id": "hg-00000000000000aa", "name": "Fake", "caps": caps})
        self.paired = True

    @property
    def messages(self) -> list[dict]:
        return [m for m in self._ws.sent if isinstance(m, dict)]

    def types(self) -> list[str]:
        return [m["type"] for m in self.messages]

    def audio(self, stream: int | None = None) -> bytes:
        """The PCM sent on ``stream``, or on every stream."""
        return b"".join(f.payload for f in self._ws.sent if not isinstance(f, dict)
                        and f.channel == protocol.CHANNEL_AUDIO and stream in (None, f.stream))
