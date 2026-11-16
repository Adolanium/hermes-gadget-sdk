"""gadget_play: a file or a stream decoded on the host into the device's paced audio stream."""

from __future__ import annotations

import asyncio
import json
import math
import os
import re
import shutil
import struct
import subprocess
import time
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

from conftest import requires_hermes, requires_sim
from fakes.fake_device import FakeDevice
from hermes_gadget_plugin import hub as hubmod
from hermes_gadget_plugin import playback, tools

RATE = 16000
# CI sets HG_REQUIRE_FFMPEG=1 after installing ffmpeg, so a missing ffmpeg fails there instead of skipping.
requires_ffmpeg = pytest.mark.skipif(
    shutil.which("ffmpeg") is None and not os.environ.get("HG_REQUIRE_FFMPEG"), reason="ffmpeg is not installed")


@pytest.mark.skipif(not os.environ.get("HG_REQUIRE_FFMPEG"), reason="ffmpeg not required here")
def test_ffmpeg_present_when_required():
    assert shutil.which("ffmpeg"), "HG_REQUIRE_FFMPEG is set but ffmpeg is not on PATH"


def _tone(seconds: float, rate: int = RATE, channels: int = 1, freq: float = 440.0) -> bytes:
    frames = (int(12000 * math.sin(2 * math.pi * freq * i / rate)) for i in range(int(seconds * rate)))
    return b"".join(struct.pack("<h", s) * channels for s in frames)


def _wav(path: Path, seconds: float, rate: int = RATE, channels: int = 1) -> Path:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(_tone(seconds, rate, channels))
    return path


def _encoded(tmp_path: Path, name: str, seconds: float) -> Path:
    """A compressed file made by the host's ffmpeg from a tone."""
    out = tmp_path / name
    subprocess.run([shutil.which("ffmpeg"), "-v", "error", "-y", "-i", str(_wav(tmp_path / "tone-src.wav", seconds)),
                    str(out)], check=True)
    return out


def _seconds(pcm: bytes, rate: int = RATE) -> float:
    return len(pcm) / 2 / rate


def _peak(pcm: bytes) -> int:
    return max(abs(s) for (s,) in struct.iter_unpack("<h", pcm))


class Body:
    """A fetched HTTP body, as playback.hermes_fetch hands it over."""

    def __init__(self, data: bytes, content_type: str = "audio/mpeg", chunk: int = 4096):
        self.content_type = content_type
        self._chunks = iter([data[i:i + chunk] for i in range(0, len(data), chunk)])
        self.closed = False

    def __iter__(self):
        return self._chunks

    def close(self) -> None:
        self.closed = True


class Endless(playback.Source):
    """A live stream: a tone for as long as anything reads it."""

    def __init__(self, rate: int = RATE):
        self.rate = rate
        self.read_s = 0.0
        self.closed = False
        self._chunk = _tone(playback.CHUNK_S, rate)

    def _read(self, wait: float) -> bytes:
        if self.closed:
            return b""
        self.read_s += playback.CHUNK_S
        return self._chunk


def _all(source: playback.Source) -> bytes:
    """Everything a source decodes, to its end."""
    pcm = b""
    while (chunk := source.read(5.0)) != b"":
        assert chunk is not None, "the source gave nothing for 5 s"
        pcm += chunk
    return pcm


async def _until(condition, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, "timed out"
        await asyncio.sleep(0.01)


# -- what may be played ------------------------------------------------------------------


@pytest.mark.parametrize("url, why", [
    ("file:///etc/passwd", "only http and https"),
    ("ftp://example.com/a.mp3", "only http and https"),
    ("rtsp://example.com/live", "only http and https"),
    ("concat://a.mp3|b.mp3", "only http and https"),
    ("https://user:secret@example.com/a.mp3", "user name or password"),
    ("http:///a.mp3", "no host"),
])
def test_only_plain_http_urls_are_fetched(url, why):
    fetched = []
    with pytest.raises(playback.PlaybackError, match=why):
        playback.open_source(url, RATE, roots=[], fetch=lambda u: fetched.append(u))
    assert fetched == []  # refused before anything was requested


def test_a_local_file_plays_only_from_an_allowed_folder(tmp_path):
    allowed, elsewhere = tmp_path / "sounds", tmp_path / "private"
    allowed.mkdir()
    elsewhere.mkdir()
    inside, outside = _wav(allowed / "chime.wav", 0.3), _wav(elsewhere / "secret.wav", 0.3)

    assert playback.check_path(str(inside), [allowed]) == inside.resolve()
    said = set()
    for refused, roots in ((outside, [allowed]), (allowed / ".." / "private" / "secret.wav", [allowed]),
                           (elsewhere / "missing.wav", [allowed]), (allowed, [allowed]), (inside, [])):
        with pytest.raises(playback.PlaybackError) as refusal:
            playback.open_source(str(refused), RATE, roots=roots)
        said.add(str(refusal.value))
    # One answer whether the path exists or not, and it names no folder on the host.
    assert said == {playback.REFUSED_PATH} and str(tmp_path) not in playback.REFUSED_PATH
    with pytest.raises(playback.PlaybackError, match="no such file in the folders"):
        playback.open_source(str(allowed / "missing.wav"), RATE, roots=[allowed])


def test_a_link_out_of_an_allowed_folder_is_not_followed(tmp_path):
    allowed, elsewhere = tmp_path / "sounds", tmp_path / "private"
    allowed.mkdir()
    elsewhere.mkdir()
    link = allowed / "innocent.wav"
    try:
        link.symlink_to(_wav(elsewhere / "secret.wav", 0.3))
    except OSError:
        pytest.skip("this account cannot create symbolic links")
    with pytest.raises(playback.PlaybackError, match="cannot be played"):
        playback.open_source(str(link), RATE, roots=[allowed])


def test_settings_bound_the_length_and_name_the_folders():
    assert playback.max_seconds({}) == playback.DEFAULT_MAX_MINUTES * 60
    assert playback.max_seconds({"play_max_minutes": 5}) == 300
    assert playback.max_seconds({"play_max_minutes": "nonsense"}) == playback.DEFAULT_MAX_MINUTES * 60
    assert playback.max_seconds({"play_max_minutes": -1}) == playback.DEFAULT_MAX_MINUTES * 60
    assert playback.play_dirs({}) == []
    assert playback.play_dirs({"play_dirs": "/srv/sounds"}) == [Path("/srv/sounds")]
    assert playback.play_dirs({"play_dirs": ["/a", "/b"]}) == [Path("/a"), Path("/b")]


@pytest.mark.parametrize("content_type, url, body, says", [
    ("audio/x-mpegurl", "http://radio.example/listen", b"#EXTM3U\nhttp://s1.example/live.mp3\n", "http://s1.example/live.mp3"),
    ("text/plain", "http://radio.example/listen.pls", b"[playlist]\nFile1=https://s2.example/a\n", "https://s2.example/a"),
    ("application/vnd.apple.mpegurl", "http://tv.example/x", b"#EXTM3U\n#EXT-X-VERSION:3\nseg0.ts\n", "HLS"),
])
def test_a_playlist_is_named_as_one_instead_of_being_decoded(content_type, url, body, says):
    fetched = Body(body, content_type)
    with pytest.raises(playback.PlaybackError, match=says):
        playback.open_source(url, RATE, roots=[], fetch=lambda _url: fetched)
    assert fetched.closed


# -- decoding ----------------------------------------------------------------------------


def test_a_wav_file_is_decoded_resampled_and_played_to_the_end(tmp_path):
    path = _wav(tmp_path / "chime.wav", 0.6, rate=44100, channels=2)

    async def run():
        device = FakeDevice(rate=RATE)
        source = playback.open_source(str(path), device.speaker_rate, roots=[tmp_path])
        played = await playback.play(device, source, title="chime.wav", max_s=60)
        await asyncio.wait_for(played.task, 5)
        await asyncio.wait_for(device._audio_out.done.wait(), 5)
        return device, played

    device, played = asyncio.run(run())
    start = next(m for m in device.messages if m["type"] == "audio.start")
    assert (start["rate"], start["format"]) == (RATE, "pcm16")
    assert device.types()[-1] == "audio.end"
    assert {"type": "notice", "text": "Playing chime.wav", "ttl_s": 5} in device.messages
    assert abs(_seconds(device.audio()) - 0.6) < 0.01   # 44.1 kHz stereo became 16 kHz mono, all of it
    assert _peak(device.audio()) > 10000
    assert played.outcome == "finished" and device.playback is None


def test_volume_lowers_the_level_of_one_playback(tmp_path):
    path = _wav(tmp_path / "chime.wav", 0.3)

    async def run(volume):
        device = FakeDevice()
        source = playback.open_source(str(path), RATE, roots=[tmp_path])
        played = await playback.play(device, source, title="chime", volume=volume, max_s=60)
        await asyncio.wait_for(played.task, 5)
        await asyncio.wait_for(device._audio_out.done.wait(), 5)
        return _peak(device.audio())

    full, quarter = asyncio.run(run(100)), asyncio.run(run(25))
    assert full > 10000 and abs(quarter - full / 4) < 0.02 * full


@requires_ffmpeg
@pytest.mark.parametrize("name", ["tone.mp3", "tone.ogg", "tone.flac"])
def test_compressed_files_are_decoded_through_ffmpeg_at_the_device_rate(tmp_path, name):
    path = _encoded(tmp_path, name, 0.8)
    source = playback.open_source(str(path), 24000, roots=[tmp_path])
    assert isinstance(source, playback.FfmpegSource) and (source.rate, source.channels) == (24000, 1)
    pcm = _all(source)
    source.close()
    assert source.error is None
    assert abs(_seconds(pcm, 24000) - 0.8) < 0.1 and _peak(pcm) > 5000


@requires_ffmpeg
def test_a_url_is_fetched_by_the_host_and_piped_into_the_decoder(tmp_path):
    body = Body(_encoded(tmp_path, "episode.mp3", 0.8).read_bytes())
    asked = []

    async def run():
        device = FakeDevice()
        source = await asyncio.to_thread(playback.open_source, "https://cdn.example/episode.mp3", RATE, roots=[],
                                         fetch=lambda url: asked.append(url) or body)
        played = await playback.play(device, source, title=playback.title_of(asked[0]), max_s=60)
        await asyncio.wait_for(played.task, 10)
        await asyncio.wait_for(device._audio_out.done.wait(), 5)
        return device

    device = asyncio.run(run())
    assert asked == ["https://cdn.example/episode.mp3"]
    assert {"type": "notice", "text": "Playing episode.mp3", "ttl_s": 5} in device.messages
    assert abs(_seconds(device.audio()) - 0.8) < 0.1 and device.types()[-1] == "audio.end"
    assert body.closed


@requires_ffmpeg
def test_what_cannot_be_decoded_is_an_error_before_anything_is_queued(tmp_path):
    junk = tmp_path / "notes.mp3"
    junk.write_bytes(b"this is not audio at all\n" * 200)
    with pytest.raises(playback.PlaybackError, match="ffmpeg could not decode it"):
        playback.open_source(str(junk), RATE, roots=[tmp_path])
    page = Body(b"<html><body>Listen live</body></html>" * 50, "text/html")
    with pytest.raises(playback.PlaybackError, match="ffmpeg could not decode it"):
        playback.open_source("https://radio.example/", RATE, roots=[], fetch=lambda _url: page)
    assert page.closed


@requires_ffmpeg
def test_fetched_data_cannot_make_the_decoder_open_files_or_urls(tmp_path):
    """A playlist served as if it were audio names a file on the Hermes computer and a URL on its
    network. Neither is read: nothing plays, and the server it names is never asked."""
    import http.server
    import threading

    secret = _wav(tmp_path / "secret.wav", 0.5)
    asked = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(self.path)
            self.send_response(200)
            self.send_header("Content-Length", str(secret.stat().st_size))
            self.end_headers()
            self.wfile.write(secret.read_bytes())

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    internal = f"http://127.0.0.1:{server.server_address[1]}/secret.wav"
    try:
        for target in (secret.as_posix(), f"file://{secret.as_posix()}", internal):
            for data in (f"ffconcat version 1.0\nfile '{target}'\n",
                         f"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\n{target}\n#EXT-X-ENDLIST\n"):
                body = Body(data.encode(), "audio/mpeg")
                with pytest.raises(playback.PlaybackError):
                    playback.open_source("https://cdn.example/track.mp3", RATE, roots=[], fetch=lambda _url: body)
    finally:
        server.shutdown()
        server.server_close()
    assert asked == []


@requires_ffmpeg
def test_a_url_sends_no_more_than_its_size_limit(tmp_path):
    data = _encoded(tmp_path, "long.mp3", 20.0).read_bytes()
    source = playback.FfmpegSource(RATE, body=Body(data), max_bytes=len(data) // 4).prime()
    pcm = _all(source)
    source.close()
    assert "the most one URL may send" in source.error
    assert 2.0 < _seconds(pcm) < 10.0  # about a quarter of it was decoded, then the stream ended


def test_without_ffmpeg_a_compressed_file_is_refused_with_the_reason(tmp_path, monkeypatch):
    monkeypatch.setattr(playback.shutil, "which", lambda _name: None)
    path = tmp_path / "song.mp3"
    path.write_bytes(b"ID3" + bytes(400))
    with pytest.raises(playback.PlaybackError, match="ffmpeg is not installed"):
        playback.open_source(str(path), RATE, roots=[tmp_path])
    assert playback.open_source(str(_wav(tmp_path / "ok.wav", 0.2)), RATE, roots=[tmp_path]).read()  # WAV still plays


# -- long playback, stop and pre-emption ------------------------------------------------


def test_a_long_stream_is_decoded_only_as_fast_as_it_is_paced_and_ends_at_the_limit(monkeypatch):
    """Ten minutes of a live stream: paced to real time, never decoded far ahead, cut at the limit."""
    clock = SimpleNamespace(now=1000.0)
    real_sleep = asyncio.sleep
    limit_s = 600.0

    async def fake_sleep(seconds, *args):
        if seconds > 0:
            clock.now += seconds
        await real_sleep(0)

    monkeypatch.setattr(hubmod.asyncio, "sleep", fake_sleep)

    async def run():
        loop = asyncio.get_running_loop()
        monkeypatch.setattr(loop, "time", lambda: clock.now)
        device = FakeDevice()
        source = Endless()
        ahead_of_sent, ahead_of_time = [], []
        read = source._read

        def watched(wait):
            out = device._audio_out
            ahead_of_sent.append(source.read_s - out.sent_seconds)
            ahead_of_time.append(out.sent_seconds - (clock.now - 1000.0))
            return read(wait)

        source._read = watched
        played = await playback.play(device, source, title="radio", max_s=limit_s)
        await played.task  # no wait_for: its timeout would run on the virtual clock
        await device._audio_out.done.wait()
        return device, source, played, max(ahead_of_sent), max(ahead_of_time)

    device, source, played, ahead_of_sent, ahead_of_time = asyncio.run(run())
    assert abs(_seconds(device.audio()) - limit_s) < 0.05 and device.types()[-1] == "audio.end"
    assert played.outcome == "stopped at the 10 minute limit" and source.closed
    assert clock.now - 1000.0 >= limit_s - hubmod.PLAYBACK_LEAD_S - 0.5        # it took the time it lasts
    assert ahead_of_time <= hubmod.PLAYBACK_LEAD_S + 0.05                      # never a burst past the lead
    assert ahead_of_sent <= playback.BACKLOG_S + hubmod.PLAYBACK_LEAD_S + 1.0  # never held more than seconds of it


def test_stop_ends_a_stream_at_once_and_closes_its_source():
    async def run():
        device = FakeDevice()
        source = Endless()
        played = await playback.play(device, source, title="radio", max_s=3600)
        await _until(lambda: len(device.audio()) > 0)
        stream = device._audio_out.stream
        began = time.monotonic()
        assert await playback.stop(device) is True
        took = time.monotonic() - began
        sent = len(device.audio())
        await asyncio.sleep(0.3)
        assert await playback.stop(device) is False  # nothing left to stop
        return device, source, played, stream, took, sent

    device, source, played, stream, took, sent = asyncio.run(run())
    assert {"type": "audio.abort", "stream": stream} in device.messages and "audio.end" not in device.types()
    assert source.closed and played.task.cancelled() and played.outcome == "stopped"
    assert took < 0.5 and len(device.audio()) == sent  # no frame after the stop
    assert device.playback is None


def test_a_spoken_reply_takes_over_from_a_playback():
    async def run():
        device = FakeDevice()
        source = Endless()
        played = await playback.play(device, source, title="radio", max_s=3600)
        await _until(lambda: len(device.audio()) > 0)
        music = device._audio_out.stream
        # A message queued to be spoken (a reminder, send_message) waits for speech, not for music.
        assert device._audio_out.playing and not device.audio_playing
        reply = await device.play_pcm(_tone(0.4), RATE, turn="t1")  # Hermes speaks: a new audio.start
        assert device.audio_playing
        await asyncio.wait_for(played.task, 5)
        music_sent = len(device.audio(music))
        await asyncio.wait_for(reply.done.wait(), 5)
        return device, source, played, music, reply, music_sent

    device, source, played, music, reply, music_sent = asyncio.run(run())
    types = device.types()
    assert types.index("audio.abort") < len(types) - 1 - types[::-1].index("audio.start")  # the abort, then the reply
    assert {"type": "audio.abort", "stream": music} in device.messages
    assert played.outcome == "interrupted" and source.closed and device.playback is None
    assert len(device.audio(music)) == music_sent                  # the music never resumed
    assert abs(_seconds(device.audio(reply.stream)) - 0.4) < 0.01  # the reply was sent whole
    assert not reply.aborted and types[-1] == "audio.end"


def test_cancel_on_the_device_and_a_new_recording_both_end_a_playback(tmp_path):
    from hermes_gadget_plugin.store import DeviceStore

    async def run(message):
        hub = hubmod.DeviceHub(DeviceStore(tmp_path / message["type"]), hubmod.HubDelegate())
        device = FakeDevice()
        device.hub = hub
        source = Endless()
        played = await playback.play(device, source, title="radio", max_s=3600)
        await _until(lambda: len(device.audio()) > 0)
        await hub._on_text(device, json.dumps(message))  # what the device sends
        await asyncio.wait({played.task}, timeout=5)
        assert played.task.done()
        for task in list(device._tasks):
            task.cancel()
        return device, source, played

    for message in ({"type": "cancel"}, {"type": "audio.start", "stream": 3, "rate": RATE, "format": "pcm16"},
                    # A recording the server refuses still means the device stopped its speaker to make it.
                    {"type": "audio.start", "stream": 4, "rate": RATE, "format": "flac"}):
        device, source, played = asyncio.run(run(message))
        assert "audio.abort" in device.types() and played.outcome == "interrupted" and source.closed


def test_cancel_on_the_device_and_a_new_recording_both_end_a_playback_that_is_still_waiting(tmp_path):
    from hermes_gadget_plugin.store import DeviceStore

    async def run(message):
        hub = hubmod.DeviceHub(DeviceStore(tmp_path / message["type"]), hubmod.HubDelegate())
        device = FakeDevice()
        device.hub = hub
        busy = asyncio.Event()

        async def idle(deadline):  # a reply is being spoken: the playback has no stream yet
            await busy.wait()
            return True

        source = Endless()
        played = await playback.play(device, source, title="radio", max_s=3600, idle=idle)
        await asyncio.sleep(0.1)
        await hub._on_text(device, json.dumps(message))  # what the device sends
        busy.set()                                       # the reply is over: nothing may start now
        await asyncio.sleep(0.3)
        for task in list(device._tasks):
            task.cancel()
        return device, source, played

    for message in ({"type": "cancel"}, {"type": "audio.start", "stream": 3, "rate": RATE, "format": "pcm16"},
                    {"type": "session.new"}):
        device, source, played = asyncio.run(run(message))
        assert "audio.start" not in device.types() and source.read_s == 0
        assert played.outcome == "interrupted" and source.closed and device.playback is None


def test_a_stop_while_the_stream_is_being_opened_leaves_the_speaker_free():
    async def run():
        device = FakeDevice(send_delay=0.3)  # audio.start takes a while to go out
        source = Endless()
        played = await playback.play(device, source, title="radio", max_s=3600)
        await _until(lambda: device._audio_out is not None)
        assert played.out is None  # still inside open_audio
        assert await playback.stop(device) is True
        opened = device._audio_out
        await asyncio.wait_for(opened.done.wait(), 2)  # no pump ever ran: only the open can end it
        return device, source, opened

    device, source, opened = asyncio.run(run())
    assert opened.aborted and source.closed and len(device.audio()) == 0


def test_a_playback_can_be_stopped_while_its_last_audio_is_still_playing(tmp_path):
    async def run():
        device = FakeDevice()
        source = playback.open_source(str(_wav(tmp_path / "chime.wav", 1.5)), device.speaker_rate, roots=[tmp_path])
        played = await playback.play(device, source, title="chime.wav", max_s=60)
        await _until(lambda: played.out is not None and played.out.closed)  # all of it decoded and queued
        stream = played.out.stream
        assert not played.out.done.is_set() and device.playback is played
        assert not device.audio_playing  # still music: a spoken reply takes over, it does not wait
        assert await playback.stop(device) is True
        sent = len(device.audio())
        await asyncio.sleep(0.3)
        return device, played, stream, sent

    device, played, stream, sent = asyncio.run(run())
    assert {"type": "audio.abort", "stream": stream} in device.messages and "audio.end" not in device.types()
    assert played.outcome == "stopped" and len(device.audio()) == sent and device.playback is None


def test_a_playback_that_plays_out_is_named_by_the_session_until_the_device_has_played_it(tmp_path):
    async def run():
        device = FakeDevice()
        source = playback.open_source(str(_wav(tmp_path / "chime.wav", 0.3)), device.speaker_rate, roots=[tmp_path])
        played = await playback.play(device, source, title="chime.wav", max_s=60)
        await _until(lambda: "audio.end" in device.types())
        named_at_the_end = device.playback is played  # sent, and up to the lead still in the device's buffer
        await asyncio.wait_for(played.task, 5)
        return device, played, named_at_the_end

    device, played, named_at_the_end = asyncio.run(run())
    assert named_at_the_end and played.outcome == "finished" and device.playback is None


def test_a_playback_waits_for_the_reply_being_spoken_and_a_new_one_replaces_it():
    async def run():
        device = FakeDevice()
        busy = asyncio.Event()

        async def idle(deadline):  # the adapter's _device_idle: the turn and its spoken reply are over
            await busy.wait()
            return True

        first, second = Endless(), Endless()
        await playback.play(device, first, title="first", max_s=3600, idle=idle)
        await asyncio.sleep(0.2)
        assert "audio.start" not in device.types() and first.read_s == 0  # nothing cut the reply off
        await playback.play(device, second, title="second", max_s=3600, idle=idle)
        busy.set()
        await _until(lambda: len(device.audio()) > 0)
        await playback.stop(device)
        return device, first, second

    device, first, second = asyncio.run(run())
    assert first.closed and first.read_s == 0 and second.read_s > 0
    assert device.types().count("audio.start") == 1
    assert [m["text"] for m in device.messages if m["type"] == "notice"] == ["Playing second"]


def test_a_device_that_stays_busy_never_starts_the_playback():
    async def run():
        device = FakeDevice()
        source = Endless()

        async def never_idle(deadline):
            return False

        played = await playback.play(device, source, title="radio", max_s=3600, idle=never_idle)
        await asyncio.wait_for(played.task, 5)
        return device, source, played

    device, source, played = asyncio.run(run())
    assert played.outcome == "the device stayed busy" and source.closed
    assert "audio.start" not in device.types() and device.playback is None


# -- the agent tool ----------------------------------------------------------------------


@pytest.fixture
def tool(monkeypatch, tmp_path):
    """gadget_play against a fake device, with the loop that would be the gateway's on a thread."""
    from conftest import LoopThread

    lt = LoopThread()
    device = lt.run(_make_device())
    idle = SimpleNamespace(calls=0)

    async def device_idle(chat_id, deadline):
        idle.calls += 1
        return True

    adapter = SimpleNamespace(loop=lt.loop, hub=None, config=SimpleNamespace(extra={"play_dirs": [str(tmp_path)]}),
                              _device_idle=device_idle)
    monkeypatch.setattr(tools, "_adapter", lambda: adapter)
    monkeypatch.setattr(tools, "_resolve", lambda _adapter, _ref: (device, None))
    monkeypatch.setattr(tools, "_play_roots", lambda extra: playback.play_dirs(extra))
    fetched = []
    monkeypatch.setattr(playback, "hermes_fetch", lambda url: fetched.append(url) or Body(b"", "text/html"))
    yield SimpleNamespace(call=lambda args: json.loads(tools.gadget_play(args)), device=device, loop=lt,
                          adapter=adapter, idle=idle, fetched=fetched)
    lt.run(playback.stop(device))
    lt.stop()


async def _make_device(**kw):
    return FakeDevice(**kw)


def test_the_tool_plays_a_file_and_stops_it(tool, tmp_path):
    path = _wav(tmp_path / "alarm.wav", 30.0)
    started = tool.call({"source": str(path), "title": "**Alarm**", "volume": 50})
    assert started["success"] is True and started["playing"] == "Alarm"
    deadline = time.monotonic() + 5
    while not tool.device.audio() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert tool.device.audio() and tool.idle.calls == 1  # it asked whether the device was free first
    assert {"type": "notice", "text": "Playing Alarm", "ttl_s": 5} in tool.device.messages
    assert 5000 < _peak(tool.device.audio()) < 7000  # half of the tone's 12000

    assert tool.call({"stop": True}) == {"success": True, "device": "Fake", "stopped": True}
    assert "audio.abort" in tool.device.types()
    assert tool.call({"stop": True})["stopped"] is False


def test_the_tool_refuses_what_it_may_not_play(tool, tmp_path):
    outside = _wav(tmp_path.parent / f"{tmp_path.name}-outside.wav", 0.2)
    try:
        for args, says in (
            ({}, "source is required"),
            ({"source": str(outside)}, "cannot be played"),
            ({"source": "file:///etc/passwd"}, "only http and https"),
            ({"source": "https://example.com/page"}, "no audio|ffmpeg"),  # fetched, and it was a web page
        ):
            answer = tool.call(args)
            assert answer["success"] is False and re.search(says, answer["error"]), answer
    finally:
        outside.unlink()
    assert tool.fetched == ["https://example.com/page"]  # the only request made
    assert tool.device.types() == [] and tool.device.playback is None


def test_the_tool_needs_a_speaker_and_a_paired_device(tool, monkeypatch, tmp_path):
    path = _wav(tmp_path / "chime.wav", 0.2)
    silent = tool.loop.run(_make_device(speaker=False))
    monkeypatch.setattr(tools, "_resolve", lambda _adapter, _ref: (silent, None))
    assert "has no speaker" in tool.call({"source": str(path)})["error"]
    silent.paired = False
    assert "waiting for pairing" in tool.call({"source": str(path)})["error"]


def test_the_tool_is_registered_with_a_schema_the_manifest_lists():
    registered = []
    tools.register_tools(SimpleNamespace(register_tool=lambda **kw: registered.append(kw)))
    play = next(t for t in registered if t["name"] == "gadget_play")
    assert play["handler"] is tools.gadget_play and play["toolset"] == "gadget"
    assert set(play["schema"]["parameters"]["properties"]) == {"source", "stop", "volume", "title", "device"}
    manifest = (Path(tools.__file__).parent / "plugin.yaml").read_text(encoding="utf-8")
    assert "  - gadget_play\n" in manifest


# -- Hermes's URL policy -----------------------------------------------------------------


@requires_hermes
@pytest.mark.parametrize("url", [
    "http://127.0.0.1:8642/v1/audio", "http://localhost/a.mp3", "http://192.168.1.10/a.mp3",
    "http://169.254.169.254/latest/meta-data/", "http://[::1]/a.mp3",
])
def test_hermes_refuses_private_and_internal_addresses(url):
    with pytest.raises(playback.PlaybackError, match="URL policy refuses"):
        playback.hermes_fetch(url)


@requires_hermes
def test_a_redirect_to_an_internal_address_is_refused_and_a_public_file_is_streamed(tmp_path, monkeypatch):
    """Over a real HTTP server on this machine, reached as Hermes's ``security.allow_private_urls``
    would allow it; the cloud metadata address stays refused whatever that setting says."""
    import http.server
    import threading

    import tools.url_safety as url_safety

    audio_file = _wav(tmp_path / "served.wav", 0.5).read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "http://169.254.169.254/latest/meta-data/")
                self.end_headers()
                return
            if self.path != "/served.wav":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(audio_file)))
            self.end_headers()
            self.wfile.write(audio_file)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        with pytest.raises(playback.PlaybackError, match="URL policy refuses"):
            playback.hermes_fetch(f"{base}/served.wav")  # Hermes's default: this machine is off limits
        monkeypatch.setattr(url_safety, "_global_allow_private_urls", lambda: True)
        with pytest.raises(playback.PlaybackError, match="redirects to an address"):
            playback.hermes_fetch(f"{base}/redirect")
        with pytest.raises(playback.PlaybackError, match="HTTP 404"):
            playback.hermes_fetch(f"{base}/missing.mp3")
        body = playback.hermes_fetch(f"{base}/served.wav")
        assert body.content_type == "audio/wav" and b"".join(body) == audio_file
        body.close()
    finally:
        server.shutdown()
        server.server_close()


@requires_hermes
@requires_sim
def test_through_hermes_a_playback_asked_for_in_a_turn_starts_after_it_and_cancel_stops_it(gadget, make_sim,
                                                                                           monkeypatch):
    """The real adapter and the simulated device: the agent calls gadget_play in the middle of a turn."""
    from gateway import session_context
    from gateway.platforms.base import get_audio_cache_dir
    from hermes_fixtures import _paired_sim

    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    monkeypatch.setattr(session_context, "get_session_env",
                        lambda name, default="": {"HERMES_SESSION_PLATFORM": "gadget",
                                                  "HERMES_SESSION_CHAT_ID": device_id}.get(name, default))
    cache = Path(get_audio_cache_dir())
    cache.mkdir(parents=True, exist_ok=True)
    path = _wav(cache / "gadget-play-test.wav", 30.0)
    outside = _wav(cache.parent / "gadget-play-outside.wav", 0.2)
    answers = []

    async def reply(event):  # the agent's turn: tool calls on a worker thread, then more work, then the reply
        for source in (str(outside), "http://127.0.0.1:9/alarm.mp3", str(path)):
            answers.append(json.loads(await asyncio.to_thread(tools.gadget_play, {"source": source})))
        await asyncio.sleep(0.6)
        return "Playing it."

    gadget.reply = reply
    try:
        sim.type_text("play the alarm")
        assert sim.wait_for(lambda: sim.last_received("audio.start") is not None, timeout=20)
        order = [m["type"] for m in sim.received]
        assert order.index("turn.end") < order.index("audio.start")  # it did not start inside the turn
        assert "cannot be played" in answers[0]["error"] and "URL policy refuses" in answers[1]["error"]
        assert answers[2]["success"] is True and answers[2]["playing"] == "gadget-play-test.wav"
        assert sim.wait_for(lambda: sim.speaker.busy(), timeout=5)
        session = gadget.adapter.hub.get(device_id)
        played = session.playback
        sim.tap("cancel")
        assert sim.wait_for(lambda: sim.last_received("audio.abort") is not None, timeout=5)
        assert sim.wait_for(lambda: played.task.done() and session.playback is None, timeout=5)
        assert played.outcome == "interrupted"
        assert json.loads(tools.gadget_play({"stop": True}))["stopped"] is False
    finally:
        path.unlink()
        outside.unlink()


# -- the simulator -----------------------------------------------------------------------


@requires_sim
def test_the_simulator_plays_a_file_and_cancel_on_the_device_stops_a_stream(loop_thread, devserver, make_sim, tmp_path):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))

    path = _wav(tmp_path / "chime.wav", 1.0, rate=44100, channels=2)
    source = playback.open_source(str(path), session.speaker_rate, roots=[tmp_path])
    played = loop_thread.run(playback.play(session, source, title="chime.wav", max_s=60))
    assert sim.wait_for(lambda: sim.last_received("audio.end") is not None, timeout=10)
    assert sim.wait_for(lambda: played.task.done(), timeout=5)  # once the device has played what it was sent
    assert played.outcome == "finished"
    assert sim.wait_for(lambda: sim.speaker.last_file is not None, timeout=10)
    with wave.open(str(sim.speaker.last_file), "rb") as heard:  # what the device's speaker was given
        assert abs(heard.getnframes() / heard.getframerate() - 1.0) < 0.05

    stream = Endless(session.speaker_rate)
    played = loop_thread.run(playback.play(session, stream, title="radio", max_s=3600))
    assert sim.wait_for(lambda: sim.speaker.busy() and stream.read_s > 1.0, timeout=5)
    assert sim.device.screen() == "responding"
    sim.tap("cancel")
    assert sim.wait_for(lambda: played.task.done(), timeout=5)
    assert played.outcome == "interrupted" and stream.closed
    assert sim.wait_for(lambda: sim.last_received("audio.abort") is not None and not sim.speaker.busy(), timeout=5)
    assert sim.wait_screen("ready", timeout=10)
