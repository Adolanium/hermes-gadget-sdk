"""Play a sound file or an audio stream on a gadget's speaker (the ``gadget_play`` tool).

A source is opened on the Hermes computer and decoded there, a chunk at a time, into the
device's ordinary paced audio stream (:class:`hub.AudioOut`), so the device needs nothing new
and a long stream is never held in memory. Plain PCM WAV is read in Python; everything else
goes through ``ffmpeg``. A URL is fetched here, under the caller's URL policy, and piped into
ffmpeg, which is given no network or file access of its own.

Whatever replaces or aborts the device's audio stream ends the playback: a spoken reply, a new
recording, CANCEL on the device, a lost connection, or ``stop()``. Nothing here waits on a source
without a limit: opening one, a stream that stops sending, and the playback as a whole each have
a wall-clock bound, and a read never holds up a stop for longer than ``POLL_S``.
"""

from __future__ import annotations

import asyncio
import collections
import logging
import queue
import re
import shutil
import subprocess
import threading
import time
import wave
from array import array
from pathlib import Path
from typing import Awaitable, Callable, Iterable, Optional, Protocol
from urllib.parse import unquote, urlsplit

from . import audio

log = logging.getLogger(__name__)

DEFAULT_MAX_MINUTES = 60          # platforms.gadget.extra.play_max_minutes
MAX_URL_BYTES = 512 * 1024 * 1024  # most that is fetched from one URL
START_WAIT_S = 120.0              # how long a playback waits for a busy device before giving up
OPEN_DEADLINE_S = 15.0            # from asking for a source to its first decoded audio
STALL_S = 20.0                    # a source that gives no audio for this long has ended
POLL_S = 0.25                     # the longest one read waits, so a stop is never far away
CHUNK_S = 0.2                     # audio decoded per read
BACKLOG_S = 1.0                   # audio decoded ahead of what the pacer has sent
PLAYLIST_BYTES = 64 * 1024
# The containers ffmpeg may read. Demuxers that open other files or URLs by name (HLS, DASH,
# concat, SDP) are not among them, so a playlist cannot lead it out of what it was handed.
FFMPEG_FORMATS = "aac,ac3,aiff,amr,ape,asf,au,caf,eac3,flac,matroska,mov,mp3,mpegts,ogg,tta,w64,wav,wv"
_PLAYLIST_HEADS = (b"#EXTM3U", b"[playlist]", b"ffconcat")
_STREAM_URL = re.compile(r"https?://[A-Za-z0-9._~:/?#\[\]@!$&()*+,;=%-]{1,200}")
REFUSED_PATH = ("that file cannot be played: local files are played only from Hermes's audio cache "
                "and the folders the owner listed in play_dirs")
_PLAYLIST_TYPES = ("audio/x-mpegurl", "audio/mpegurl", "application/vnd.apple.mpegurl",
                   "application/x-mpegurl", "audio/x-scpls", "application/pls+xml")
_PLAYLIST_SUFFIXES = (".m3u", ".m3u8", ".pls")


class PlaybackError(Exception):
    """Why a source was refused or could not be played, worded for the agent."""


class Fetch(Protocol):
    """An open HTTP response: its body in chunks, and the content type the server gave."""

    content_type: str

    def __iter__(self): ...

    def close(self) -> None: ...


def is_url(source: str) -> bool:
    return "://" in source


def check_url(url: str) -> str:
    """The URL when its form is allowed: http or https, with a host and no credentials.
    Where it may point (private and internal addresses) is the fetcher's policy."""
    try:
        parts = urlsplit(url)
        host = parts.hostname
    except ValueError:
        raise PlaybackError("that is not a valid URL") from None
    if parts.scheme.lower() not in ("http", "https"):
        raise PlaybackError(f"only http and https URLs can be played, not {parts.scheme or 'this'}://")
    if not host:
        raise PlaybackError("the URL has no host")
    if parts.username is not None or parts.password is not None:
        raise PlaybackError("a URL with a user name or password in it is not played")
    return url


def check_path(source: str, roots: Iterable[Path]) -> Path:
    """The file ``source`` names, when it lies inside one of ``roots`` once links and ``..``
    are followed. Nothing outside them is opened, whatever the path looks like, and the refusal
    is the same whether or not the path exists, so it says nothing about the rest of the host."""
    allowed = []
    for root in roots:
        try:
            allowed.append(Path(root).expanduser().resolve())
        except (OSError, RuntimeError):
            continue
    try:
        path = Path(source).expanduser().resolve()
    except (OSError, RuntimeError, ValueError):
        raise PlaybackError(REFUSED_PATH) from None
    if not any(root in path.parents for root in allowed):
        raise PlaybackError(REFUSED_PATH)
    if not path.is_file():
        raise PlaybackError("there is no such file in the folders local files are played from")
    return path


def title_of(source: str) -> str:
    """A short name for the on-screen notice: the file name, or the URL's file name or host."""
    if is_url(source):
        parts = urlsplit(source)
        name = unquote(parts.path.rstrip("/").rsplit("/", 1)[-1])
        return (name if "." in name else parts.hostname or "audio")[:40]
    return Path(source).name[:40]


def max_seconds(extra: dict) -> float:
    """The longest one playback runs (``play_max_minutes``); a bad or missing value gives the default."""
    try:
        minutes = float(extra.get("play_max_minutes"))
    except (TypeError, ValueError):
        minutes = DEFAULT_MAX_MINUTES
    return (minutes if minutes > 0 else DEFAULT_MAX_MINUTES) * 60


def play_dirs(extra: dict) -> list[Path]:
    """The folders ``play_dirs`` names; one path or a list."""
    given = extra.get("play_dirs") or []
    return [Path(str(d)) for d in ([given] if isinstance(given, str) else given) if str(d).strip()]


def scaled(pcm: bytes, percent: int) -> bytes:
    """PCM16 at ``percent`` of its level."""
    return audio._to_bytes(array("h", (s * percent // 100 for s in audio._samples(pcm))))


# -- sources ---------------------------------------------------------------------------


class Source:
    """Decoded audio, read a chunk at a time: PCM16 at ``rate`` with ``channels`` channels."""

    rate = 16000
    channels = 1
    error: Optional[str] = None  # why it ended early, once read() has returned b""
    closed = False
    _head = b""

    def prime(self) -> "Source":
        """Decode the first chunk now, so a source that cannot be played fails before it is queued.
        Waits until audio arrives, the source ends, or it is closed (open_source's deadline)."""
        chunk = None
        while chunk is None and not self.closed:
            chunk = self._read(POLL_S)
        if not chunk:
            self.close()
            raise PlaybackError(self.error or "there is no audio in it")
        self._head = chunk
        return self

    def read(self, wait: float = 0.0) -> Optional[bytes]:
        """The next chunk; ``b""`` at the end; None when a live source had nothing within ``wait`` seconds."""
        head, self._head = self._head, b""
        return head or self._read(wait)

    def _read(self, wait: float) -> Optional[bytes]:
        raise NotImplementedError

    def close(self) -> None:
        self.closed = True


class WavSource(Source):
    """A plain PCM16 WAV file, read without ffmpeg. Raises ``wave.Error`` for any other file."""

    def __init__(self, path: Path):
        self._wav = wave.open(str(path), "rb")
        if self._wav.getsampwidth() != 2 or self._wav.getcomptype() != "NONE":
            self._wav.close()
            raise wave.Error("not PCM16")
        self.rate, self.channels = self._wav.getframerate(), self._wav.getnchannels()
        self._frames = max(1, int(self.rate * CHUNK_S))

    def _read(self, wait: float) -> Optional[bytes]:
        try:
            return self._wav.readframes(self._frames)
        except (wave.Error, EOFError, OSError, ValueError) as exc:  # truncated, or closed by a stop
            if not self.closed:
                log.info("WAV read failed: %s", exc)
                self.error = self.error or "the WAV file could not be read"
            return b""

    def close(self) -> None:
        self.closed = True
        self._wav.close()


class FfmpegSource(Source):
    """Anything ffmpeg decodes, as mono PCM16 at ``rate``: a file, or a fetched body piped in.

    ffmpeg may open only what it is given (``-protocol_whitelist``) and only as an audio
    container (``-format_whitelist``), so a playlist or container that names other files or URLs
    cannot make it read or fetch them. A thread reads its output into a queue two chunks deep:
    ffmpeg blocks on its pipe once that is full, and so does the fetch behind it, while a read
    here always returns within the time it was given.
    """

    def __init__(self, rate: int, *, path: Optional[Path] = None, body: Optional[Fetch] = None,
                 max_bytes: int = MAX_URL_BYTES):
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            if body is not None:
                body.close()
            raise PlaybackError("ffmpeg is not installed on the Hermes computer; without it only PCM WAV files play")
        self.rate = rate
        self._chunk = max(1, int(rate * CHUNK_S)) * 2
        self._ended = False
        given = ["-protocol_whitelist", "pipe", "-format_whitelist", FFMPEG_FORMATS, "-i", "pipe:0"] \
            if body is not None else ["-protocol_whitelist", "file", "-format_whitelist", FFMPEG_FORMATS, "-i", str(path)]
        self._proc = subprocess.Popen(
            [ffmpeg, "-nostdin", "-v", "error", *given, "-vn", "-f", "s16le", "-acodec", "pcm_s16le",
             "-ac", "1", "-ar", str(rate), "pipe:1"],
            stdin=subprocess.PIPE if body is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self._stderr: collections.deque[bytes] = collections.deque(maxlen=4)
        self._chunks: queue.Queue[bytes] = queue.Queue(maxsize=2)
        threading.Thread(target=self._drain_stderr, daemon=True).start()
        threading.Thread(target=self._decode, daemon=True).start()
        if body is not None:
            threading.Thread(target=self._feed, args=(body, max_bytes), daemon=True).start()

    def _drain_stderr(self) -> None:
        for line in self._proc.stderr:
            self._stderr.append(line)
        self._proc.stderr.close()

    def _feed(self, body: Fetch, max_bytes: int) -> None:
        sent = 0
        try:
            for chunk in body:
                sent += len(chunk)
                if sent > max_bytes:
                    self.error = f"stopped after {max_bytes // (1024 * 1024)} MB, the most one URL may send"
                    break
                if self.closed:
                    break
                self._proc.stdin.write(chunk)
                self._proc.stdin.flush()  # a small chunk must reach ffmpeg now, or fail now if it is gone
        except OSError:
            pass  # ffmpeg went away: stopped, or it gave up on the data
        except Exception as exc:  # the connection dropped or timed out
            if not self.closed:
                log.info("download stopped: %s", exc)
                self.error = "the download stopped"
        finally:
            try:
                self._proc.stdin.close()
            except OSError:
                pass
            body.close()

    def _decode(self) -> None:
        """ffmpeg's output into the queue, then ``b""``; waits while the queue is full."""
        try:
            while data := self._proc.stdout.read(self._chunk):
                self._put(data)
        except (OSError, ValueError):
            pass
        if not self.closed and not self.error and self._proc.wait() != 0:
            # What ffmpeg said can quote the data it was given: the log keeps it, the agent is not told.
            log.info("ffmpeg failed: %s", b" ".join(self._stderr).decode(errors="replace").strip()[-400:])
            self.error = "ffmpeg could not decode it: not audio, or not a format it may read"
        self._put(b"")
        self._proc.stdout.close()

    def _put(self, data: bytes) -> None:
        while not self.closed:
            try:
                return self._chunks.put(data, timeout=POLL_S)
            except queue.Full:
                pass

    def _read(self, wait: float) -> Optional[bytes]:
        if self.closed or self._ended:
            return b""
        try:
            data = self._chunks.get(timeout=wait) if wait > 0 else self._chunks.get_nowait()
        except queue.Empty:
            return None
        self._ended = not data
        return data

    @property
    def running(self) -> bool:
        """ffmpeg has not exited."""
        return self._proc.poll() is None

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self._proc.kill()
        except OSError:
            pass
        self._proc.wait()


def _playlist_error(url: str, fetch: Fetch, abandoned: threading.Event) -> Optional[str]:
    """What to tell the agent when ``url`` is a playlist rather than audio, else None."""
    kind = (fetch.content_type or "").split(";")[0].strip().lower()
    if kind not in _PLAYLIST_TYPES and not urlsplit(url).path.lower().endswith(_PLAYLIST_SUFFIXES):
        return None
    text = bytearray()
    for chunk in fetch:
        text += chunk
        if len(text) >= PLAYLIST_BYTES or abandoned.is_set():
            break
    lines = bytes(text[:PLAYLIST_BYTES]).decode(errors="replace").splitlines()
    if any(line.startswith("#EXT-X-") for line in lines):
        return "that is an HLS stream (.m3u8), which is not supported; use a direct MP3, AAC or Ogg stream URL"
    # The entries are the remote server's text: only whole, plain URLs of bounded length are passed on.
    entries = [line.partition("=")[2] if line.lower().startswith("file") else line for line in map(str.strip, lines)]
    entries = [e.strip() for e in entries if _STREAM_URL.fullmatch(e.strip())][:3]
    if not entries:
        return "that URL is a playlist, not audio, and lists no stream URLs"
    return "that URL is a playlist, not audio. Stream URLs it lists (text from the remote server): " + " ".join(entries)


def _is_playlist_file(path: Path) -> bool:
    if path.suffix.lower() in _PLAYLIST_SUFFIXES:
        return True
    try:
        with open(path, "rb") as f:
            head = f.read(16).lstrip(b"\xef\xbb\xbf \t\r\n")
    except OSError:
        return False
    return head.startswith(_PLAYLIST_HEADS)


def open_source(source: str, rate: int, *, roots: Iterable[Path],
                fetch: Optional[Callable[[str], Fetch]] = None, deadline_s: float = OPEN_DEADLINE_S) -> Source:
    """Open ``source`` (a local path or an http(s) URL) for a device whose speaker runs at
    ``rate``, checked and with its first chunk decoded. Raises :class:`PlaybackError`.

    ``roots`` are the only folders a local file is read from. ``fetch`` opens a URL under the
    host's URL policy; without one, URLs are refused. Blocking, for at most ``deadline_s``: a
    server that answers slowly or sends nothing playable in that time is given up on, and what
    was started for it (the fetch, ffmpeg) is closed. Call it off the event loop.
    """
    lock, done, abandoned = threading.Lock(), threading.Event(), threading.Event()
    opening: dict = {}  # "started": a source being primed; then "source" or "error"

    def begin(body: Optional[Fetch] = None, path: Optional[Path] = None) -> Source:
        started = opening["started"] = FfmpegSource(rate, body=body, path=path)
        if abandoned.is_set():  # given up on while ffmpeg was starting
            started.close()
        return started.prime()

    def work() -> Source:
        if not is_url(source):
            path = check_path(source, roots)
            if _is_playlist_file(path):
                raise PlaybackError("playlists are not played; name the audio file itself")
            try:
                return WavSource(path).prime()
            except (wave.Error, EOFError):
                return begin(path=path)  # not a plain PCM WAV
        check_url(source)
        if fetch is None:
            raise PlaybackError("URLs cannot be played here")
        body = fetch(source)
        try:
            playlist = _playlist_error(source, body, abandoned)
            if abandoned.is_set():
                raise PlaybackError("given up on")
        except BaseException:
            body.close()
            raise
        if playlist:
            body.close()
            raise PlaybackError(playlist)
        return begin(body=body)

    def run() -> None:
        result: dict = {}
        try:
            result["source"] = work()
        except PlaybackError as exc:
            result["error"] = exc
        except Exception as exc:
            log.info("could not open a source: %s", exc)
            result["error"] = PlaybackError("it could not be opened")
        with lock:
            if abandoned.is_set():
                if "source" in result:
                    result["source"].close()
                return
            opening.update(result)
            done.set()

    threading.Thread(target=run, daemon=True).start()
    if not done.wait(deadline_s):
        with lock:
            if not done.is_set():
                abandoned.set()
    if abandoned.is_set():
        if (started := opening.get("started")) is not None:
            started.close()  # ends its ffmpeg, and the fetch feeding it at its next chunk
        raise PlaybackError(f"no audio arrived within {deadline_s:g} seconds")
    if "error" in opening:
        raise opening["error"]
    return opening["source"]


def hermes_fetch(url: str) -> Fetch:
    """Open ``url`` with Hermes's own guarded HTTP client: private and internal addresses are
    refused (unless Hermes's ``security.allow_private_urls`` allows them), at connect time and
    on every redirect, and cloud metadata addresses always. Behind an HTTP(S) proxy Hermes leaves
    name resolution to the proxy, so the connect-time check does not apply there."""
    import httpx
    from tools.url_safety import create_ssrf_safe_client, is_safe_url, redirect_target_from_response

    if not is_safe_url(url):
        raise PlaybackError("Hermes's URL policy refuses that address (private, internal or malformed)")

    def guard(response) -> None:
        target = redirect_target_from_response(response)
        if target and (urlsplit(target).scheme.lower() not in ("http", "https") or not is_safe_url(target)):
            raise PlaybackError("the URL redirects to an address Hermes's URL policy refuses")

    client = create_ssrf_safe_client(timeout=httpx.Timeout(10.0), follow_redirects=True,
                                     event_hooks={"response": [guard]})
    try:
        stream = client.stream("GET", url, headers={"Accept": "audio/*, */*;q=0.5"})
        response = stream.__enter__()
        if response.status_code >= 400:
            raise PlaybackError(f"the server answered HTTP {response.status_code}")
    except PlaybackError:
        client.close()
        raise
    except Exception as exc:  # DNS, connect, TLS, timeout, a refused connect-time address
        client.close()
        log.info("could not fetch a URL: %s", exc)
        raise PlaybackError(f"could not fetch it ({type(exc).__name__})") from None
    return _HttpxFetch(client, stream, response)


class _HttpxFetch:
    def __init__(self, client, stream, response):
        self._client, self._stream, self._response = client, stream, response
        self.content_type = response.headers.get("content-type", "")
        # As it arrives: a block size here makes httpx hold the bytes until the block is full, and
        # a 24 kbit/s stream needs 22 seconds to fill 64 KB.
        self._chunks = response.iter_bytes()

    def __iter__(self):
        return self._chunks

    def close(self) -> None:
        try:
            self._stream.__exit__(None, None, None)
        finally:
            self._client.close()


# -- playback --------------------------------------------------------------------------


class Playback:
    """One source playing on one device. ``outcome`` says how it ended, once ``task`` is done."""

    def __init__(self, session, source: Source, *, title: str, volume: int = 100, max_s: float,
                 idle: Optional[Callable[[float], Awaitable[bool]]] = None):
        self.session, self.source, self.title = session, source, title
        self.volume = min(100, max(1, volume))
        self.max_s = max_s
        self._idle = idle
        self.out = None  # the device's audio stream, once it has started
        self.outcome = ""
        self.task: Optional[asyncio.Task] = None

    def end(self, outcome: str) -> None:
        """End it now, from outside its task: a stop, or the device's connection closing. The
        source is closed here because a task cancelled before it first ran never reaches its
        own cleanup, and because closing is what ends a read that is waiting."""
        self.outcome = self.outcome or outcome
        if self.task is not None:
            self.task.cancel()
        self.source.close()

    async def _run(self) -> None:
        session, src = self.session, self.source
        loop = asyncio.get_running_loop()
        problem = ""
        try:
            # A reply that is still being spoken would be cut off, and the reply to the turn that
            # asked for this would cut it off in turn: start once the device has nothing to say.
            if self._idle is not None and not await self._idle(time.monotonic() + START_WAIT_S):
                self.outcome = "the device stayed busy"
                return
            if getattr(session, "closed", False):
                self.outcome = "the device disconnected"
                return
            out = self.out = await session.open_audio(src.rate, src.channels)
            await session.send_notice(f"Playing {self.title}", ttl_s=5)
            limit = f"stopped at the {self.max_s / 60:g} minute limit"
            ended = ""
            budget = int(self.max_s * src.rate) * 2 * src.channels  # of audio
            ends_at = heard_at = loop.time()
            ends_at += self.max_s                                    # and of time, however slowly it arrives
            while not out.done.is_set():
                if loop.time() >= ends_at:
                    ended = limit
                    break
                # A short wait, so whatever ends the stream (a reply, a disconnect) is seen at once.
                chunk = await asyncio.to_thread(src.read, POLL_S)
                if chunk is None:
                    if loop.time() - heard_at > STALL_S:
                        ended = problem = "the stream stopped sending audio"
                        break
                    continue
                if not chunk:
                    problem = src.error or ""
                    ended = problem or ("stopped" if src.closed else "finished")
                    break
                heard_at = loop.time()
                if self.volume < 100:
                    chunk = scaled(chunk, self.volume)
                out.write(chunk[:budget])
                budget -= len(chunk)
                if budget <= 0:
                    ended = limit
                    break
                # The pacer sends in real time: decode no further ahead than it needs.
                while out.backlog_s > BACKLOG_S and not out.done.is_set():
                    await asyncio.sleep(0.1)
            out.finish()
            # The last of it is queued, not played: it stays this playback's until the device has
            # played it, so a stop ends what is not sent yet. What the device already holds after
            # audio.end plays out: the firmware ignores an abort for a stream it has ended.
            await out.wait_played()
            if out.aborted:  # a spoken reply, a new recording or CANCEL on the device
                self.outcome = "interrupted"
            elif out.failed or getattr(session, "closed", False):
                self.outcome = "the device disconnected"
            else:
                self.outcome = ended
                if problem:
                    await session.send_notice(f"Playback stopped: {problem}"[:120])
        except asyncio.CancelledError:
            self.outcome = self.outcome or "stopped"
            raise
        finally:
            src.close()
            if getattr(session, "playback", None) is self:
                session.playback = None
            log.info("[%s] playback of %s: %s", session.device_id, self.title, self.outcome or "ended")


async def play(session, source: Source, *, title: str, volume: int = 100, max_s: float,
               idle: Optional[Callable[[float], Awaitable[bool]]] = None) -> Playback:
    """Play ``source`` on the device, replacing a playback already there. Returns at once; the
    audio starts when ``idle`` says the device is free, and runs until it ends or is stopped.

    One start or stop at a time per device: stopping the old playback waits on the socket, and a
    second start let in meanwhile would take the speaker while the session still named the first."""
    async with session.playback_lock:
        await _stop(session)
        playback = Playback(session, source, title=title, volume=volume, max_s=max_s, idle=idle)
        session.playback = playback
        playback.task = session.spawn(playback._run())
    return playback


async def stop(session) -> bool:
    """Stop the device's playback, waiting or playing. False when there was none."""
    async with session.playback_lock:
        return await _stop(session)


async def _stop(session) -> bool:
    playback: Optional[Playback] = session.playback
    if playback is None:
        return False
    session.playback = None
    playback.end("stopped")
    if playback.out is not None:
        await playback.out.abort()
    return True
