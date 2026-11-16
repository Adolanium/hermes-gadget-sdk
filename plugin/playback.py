"""Play a sound file or an audio stream on a gadget's speaker (the ``gadget_play`` tool).

A source is opened on the Hermes computer and decoded there, a chunk at a time, into the
device's ordinary paced audio stream (:class:`hub.AudioOut`), so the device needs nothing new
and a long stream is never held in memory. Plain PCM WAV is read in Python; everything else
goes through ``ffmpeg``. A URL is fetched here, under the caller's URL policy, and piped into
ffmpeg, which is given no network or file access of its own.

Whatever replaces or aborts the device's audio stream ends the playback: a spoken reply, a new
recording, CANCEL on the device, or ``stop()``.
"""

from __future__ import annotations

import asyncio
import collections
import logging
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
CHUNK_S = 0.2                     # audio decoded per read
BACKLOG_S = 1.0                   # audio decoded ahead of what the pacer has sent
PLAYLIST_BYTES = 64 * 1024
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
    are followed. Nothing outside them is opened, whatever the path looks like."""
    allowed = []
    for root in roots:
        try:
            allowed.append(Path(root).expanduser().resolve())
        except (OSError, RuntimeError):
            continue
    where = ", ".join(str(r) for r in allowed) or "nowhere (set platforms.gadget.extra.play_dirs)"
    try:
        path = Path(source).expanduser().resolve(strict=True)
    except (OSError, RuntimeError, ValueError):
        raise PlaybackError(f"no such file: {source}. Local files are played only from: {where}") from None
    if not any(path == root or root in path.parents for root in allowed):
        raise PlaybackError(f"{source} is outside the folders local files are played from: {where}")
    if not path.is_file():
        raise PlaybackError(f"{source} is not a file")
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
    _head = b""

    def prime(self) -> "Source":
        """Decode the first chunk now, so a source that cannot be played fails before it is queued."""
        self._head = self._read()
        if not self._head:
            self.close()
            raise PlaybackError(self.error or "there is no audio in it")
        return self

    def read(self) -> bytes:
        """The next chunk, or ``b""`` at the end. Blocks while a live stream has nothing new."""
        head, self._head = self._head, b""
        return head or self._read()

    def _read(self) -> bytes:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class WavSource(Source):
    """A plain PCM16 WAV file, read without ffmpeg. Raises ``wave.Error`` for any other file."""

    def __init__(self, path: Path):
        self._wav = wave.open(str(path), "rb")
        if self._wav.getsampwidth() != 2 or self._wav.getcomptype() != "NONE":
            self._wav.close()
            raise wave.Error("not PCM16")
        self.rate, self.channels = self._wav.getframerate(), self._wav.getnchannels()
        self._frames = max(1, int(self.rate * CHUNK_S))

    def _read(self) -> bytes:
        try:
            return self._wav.readframes(self._frames)
        except (wave.Error, EOFError, OSError, ValueError) as exc:  # truncated, or closed by a stop
            self.error = self.error or f"the WAV file could not be read: {exc}"
            return b""

    def close(self) -> None:
        self._wav.close()


class FfmpegSource(Source):
    """Anything ffmpeg decodes, as mono PCM16 at ``rate``: a file, or a fetched body piped in.

    ffmpeg may open only what it is given (``-protocol_whitelist``), so a playlist or container
    that names other files or URLs cannot make it read or fetch them. Output is read as it is
    played: ffmpeg blocks on its pipe meanwhile, and so does the fetch behind it.
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
        self._closed = False
        given = ["-protocol_whitelist", "pipe", "-i", "pipe:0"] if body is not None else [
            "-protocol_whitelist", "file", "-i", str(path)]
        self._proc = subprocess.Popen(
            [ffmpeg, "-nostdin", "-v", "error", *given, "-vn", "-f", "s16le", "-acodec", "pcm_s16le",
             "-ac", "1", "-ar", str(rate), "pipe:1"],
            stdin=subprocess.PIPE if body is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self._stderr: collections.deque[bytes] = collections.deque(maxlen=4)
        threading.Thread(target=self._drain_stderr, daemon=True).start()
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
                self._proc.stdin.write(chunk)
        except OSError:
            pass  # ffmpeg went away: stopped, or it gave up on the data
        except Exception as exc:  # the connection dropped or timed out
            if not self._closed:
                self.error = f"the download stopped: {exc}"
        finally:
            try:
                self._proc.stdin.close()
            except OSError:
                pass
            body.close()

    def _read(self) -> bytes:
        try:
            data = self._proc.stdout.read(self._chunk)
        except (OSError, ValueError):
            data = b""
        if not data and not self._closed and not self.error:
            if self._proc.wait() != 0:
                said = b" ".join(self._stderr).decode(errors="replace").strip()
                self.error = f"ffmpeg could not decode it: {said[-200:] or 'unknown error'}"
        return data

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._proc.kill()
        except OSError:
            pass
        self._proc.wait()
        self._proc.stdout.close()  # a read in another thread has returned: its pipe is gone


def _playlist_error(url: str, fetch: Fetch) -> Optional[str]:
    """What to tell the agent when ``url`` is a playlist rather than audio, else None."""
    kind = (fetch.content_type or "").split(";")[0].strip().lower()
    if kind not in _PLAYLIST_TYPES and not urlsplit(url).path.lower().endswith(_PLAYLIST_SUFFIXES):
        return None
    text = bytearray()
    for chunk in fetch:
        text += chunk
        if len(text) >= PLAYLIST_BYTES:
            break
    lines = bytes(text[:PLAYLIST_BYTES]).decode(errors="replace").splitlines()
    if any(line.startswith("#EXT-X-") for line in lines):
        return "that is an HLS stream (.m3u8), which is not supported; use a direct MP3, AAC or Ogg stream URL"
    entries = [line.partition("=")[2] if line.lower().startswith("file") else line for line in map(str.strip, lines)]
    entries = [e.strip() for e in entries if e.strip().lower().startswith(("http://", "https://"))][:5]
    if not entries:
        return "that is a playlist with no stream URLs in it"
    return "that is a playlist, not audio. Play one of its entries instead: " + ", ".join(entries)


def open_source(source: str, rate: int, *, roots: Iterable[Path],
                fetch: Optional[Callable[[str], Fetch]] = None) -> Source:
    """Open ``source`` (a local path or an http(s) URL) for a device whose speaker runs at
    ``rate``, checked and with its first chunk decoded. Raises :class:`PlaybackError`.

    ``roots`` are the only folders a local file is read from. ``fetch`` opens a URL under the
    host's URL policy; without one, URLs are refused. Blocking: call it off the event loop.
    """
    if is_url(source):
        check_url(source)
        if fetch is None:
            raise PlaybackError("URLs cannot be played here")
        body = fetch(source)
        try:
            playlist = _playlist_error(source, body)
        except Exception as exc:
            body.close()
            raise PlaybackError(f"could not read {source}: {exc}") from None
        if playlist:
            body.close()
            raise PlaybackError(playlist)
        return FfmpegSource(rate, body=body).prime()
    path = check_path(source, roots)
    try:
        return WavSource(path).prime()
    except (wave.Error, EOFError):
        pass  # not a plain PCM WAV
    return FfmpegSource(rate, path=path).prime()


def hermes_fetch(url: str) -> Fetch:
    """Open ``url`` with Hermes's own guarded HTTP client: private and internal addresses are
    refused (unless Hermes's ``security.allow_private_urls`` allows them), at connect time and
    on every redirect, and cloud metadata addresses always."""
    import httpx
    from tools.url_safety import create_ssrf_safe_client, is_safe_url, redirect_target_from_response

    if not is_safe_url(url):
        raise PlaybackError("Hermes's URL policy refuses that address (private, internal or malformed)")

    def guard(response) -> None:
        target = redirect_target_from_response(response)
        if target and (urlsplit(target).scheme.lower() not in ("http", "https") or not is_safe_url(target)):
            raise PlaybackError("the URL redirects to an address Hermes's URL policy refuses")

    client = create_ssrf_safe_client(timeout=httpx.Timeout(15.0), follow_redirects=True,
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
        raise PlaybackError(f"could not fetch it: {exc}") from None
    return _HttpxFetch(client, stream, response)


class _HttpxFetch:
    def __init__(self, client, stream, response):
        self._client, self._stream, self._response = client, stream, response
        self.content_type = response.headers.get("content-type", "")
        self._chunks = response.iter_bytes(64 * 1024)

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

    async def _run(self) -> None:
        session, src = self.session, self.source
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
            budget = int(self.max_s * src.rate) * 2 * src.channels
            while not out.done.is_set():
                chunk = await asyncio.to_thread(src.read)
                if not chunk:
                    self.outcome = src.error or "finished"
                    break
                if self.volume < 100:
                    chunk = scaled(chunk, self.volume)
                out.write(chunk[:budget])
                budget -= len(chunk)
                if budget <= 0:
                    self.outcome = f"stopped at the {self.max_s / 60:g} minute limit"
                    break
                # The pacer sends in real time: decode no further ahead than it needs.
                while out.backlog_s > BACKLOG_S and not out.done.is_set():
                    await asyncio.sleep(0.1)
            out.finish()
            if out.aborted:  # a spoken reply, a new recording or CANCEL on the device
                self.outcome = "interrupted"
            elif out.failed or getattr(session, "closed", False):
                self.outcome = "the device disconnected"
            elif src.error:
                await session.send_notice(f"Playback stopped: {src.error}"[:120])
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
    audio starts when ``idle`` says the device is free, and runs until it ends or is stopped."""
    await stop(session)
    playback = Playback(session, source, title=title, volume=volume, max_s=max_s, idle=idle)
    session.playback = playback
    playback.task = session.spawn(playback._run())
    return playback


async def stop(session) -> bool:
    """Stop the device's playback, waiting or playing. False when there was none."""
    playback: Optional[Playback] = getattr(session, "playback", None)
    if playback is None:
        return False
    session.playback = None
    playback.outcome = "stopped"
    if playback.out is not None:
        await playback.out.abort()
    if playback.task is not None:
        playback.task.cancel()  # it may be waiting for the device, or in a read
    playback.source.close()     # now, not when the task next runs: ends a blocked read
    return True
