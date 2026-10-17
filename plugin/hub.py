"""Device hub: the WebSocket endpoint gadgets connect to.

Owns transport, authentication, enrollment, heartbeats, audio upload assembly,
paced audio playback, images and device actions. It knows nothing about
Hermes: a ``HubDelegate`` decides what a device message means. The Hermes
platform adapter is one delegate; the standalone development server
(``hermes-gadget devserver``) is another, so both run this exact code.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import secrets
import time
import uuid
from dataclasses import dataclass, field
from typing import Any

from . import audio, protocol
from .store import DeviceStore

log = logging.getLogger("hermes_gadget.hub")

HANDSHAKE_TIMEOUT_S = 10.0
AUDIO_FRAME_MS = 40          # outbound audio frame duration
PLAYBACK_LEAD_S = 0.5        # how far ahead of real time playback audio is sent
IMAGE_CHUNK_BYTES = 4096
MIN_UTTERANCE_S = 0.25
MAX_FRAME_BYTES = 1 << 20
MAX_PENDING_DEVICES = 16     # unapproved devices the store keeps, in total...
MAX_PENDING_PER_ADDRESS = 4  # ...and from one network address


class HubDelegate:
    """Decides what device traffic means. Every method has a no-op default."""

    async def is_paired(self, session: "DeviceSession") -> bool:
        return True

    async def on_ready(self, session: "DeviceSession") -> None:
        pass

    async def on_text(self, session: "DeviceSession", msg_id: str, text: str) -> None:
        pass

    async def on_utterance(self, session: "DeviceSession", msg_id: str, wav: bytes, seconds: float) -> None:
        pass

    async def on_cancel(self, session: "DeviceSession") -> None:
        pass

    async def on_new_session(self, session: "DeviceSession") -> None:
        pass

    async def on_prompt_reply(self, session: "DeviceSession", prompt_id: str, yes: bool) -> None:
        pass

    async def on_event(self, session: "DeviceSession", name: str, data: Any, notify: bool) -> None:
        pass

    async def on_state(self, session: "DeviceSession", sensors: dict) -> None:
        pass

    async def on_disconnect(self, session: "DeviceSession") -> None:
        pass


class Rejected(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class ActionError(RuntimeError):
    pass


@dataclass
class _Upload:
    id: str
    stream: int
    rate: int
    started: float
    pcm: bytearray = field(default_factory=bytearray)
    next_seq: int = 0
    gaps: int = 0
    truncated: bool = False


class AudioOut:
    """One outbound audio stream, resampled to the device rate and paced.

    Frames are sent at most ``PLAYBACK_LEAD_S`` ahead of real time so a device
    needs only a small jitter buffer. ``finish()`` returns immediately; the
    pump sends the tail and ``audio.end`` in the background.
    """

    def __init__(self, session: "DeviceSession", stream: int, src_rate: int, channels: int = 1):
        self._session = session
        self.stream = stream
        self.rate = session.speaker_rate
        self._resampler = audio.Resampler(src_rate, self.rate, channels)
        self._frame_bytes = max(2, self.rate * 2 * AUDIO_FRAME_MS // 1000)
        self._buf = bytearray()
        self._queue: asyncio.Queue[bytes | None] = asyncio.Queue()
        self._task: asyncio.Task | None = None
        self.closed = False
        self.aborted = False
        self.sent_seconds = 0.0
        self.done = asyncio.Event()

    async def start(self, *, turn: str | None = None) -> None:
        await self._session.send_json(
            protocol.message("audio.start", stream=self.stream, rate=self.rate, format="pcm16", turn=turn))
        self._task = asyncio.create_task(self._pump())

    def write(self, pcm: bytes) -> None:
        if self.closed:
            return
        self._buf += self._resampler.process(pcm)
        fb = self._frame_bytes
        while len(self._buf) >= fb:
            self._queue.put_nowait(bytes(self._buf[:fb]))
            del self._buf[:fb]

    def finish(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self._buf:
            self._queue.put_nowait(bytes(self._buf))
            self._buf.clear()
        self._queue.put_nowait(None)

    async def abort(self) -> None:
        if self.aborted:
            return
        self.closed = self.aborted = True
        if self._task:
            self._task.cancel()
        try:
            await self._session.send_json(protocol.message("audio.abort", stream=self.stream))
        except Exception:
            pass
        self.done.set()

    async def _pump(self) -> None:
        loop = asyncio.get_running_loop()
        seq = 0
        base = None
        try:
            while True:
                frame = await self._queue.get()
                if frame is None:
                    break
                if base is None:
                    # Clock starts at the first audio frame, not at pump start:
                    # TTS time-to-first-audio is not a stall and must not rebase.
                    base = loop.time()
                ahead = self.sent_seconds - (loop.time() - base)
                if ahead < 0:
                    # The producer stalled: playback is caught up, so restart the clock here.
                    # (Moving `base` by `ahead` went the wrong way and let the rest of the
                    # reply out in one burst after the first stall.)
                    base = loop.time() - self.sent_seconds
                elif ahead > PLAYBACK_LEAD_S:
                    await asyncio.sleep(ahead - PLAYBACK_LEAD_S)
                await self._session.send_binary(protocol.binary(protocol.CHANNEL_AUDIO, self.stream, seq, frame))
                seq += 1
                self.sent_seconds += len(frame) / 2 / self.rate
            await self._session.send_json(protocol.message("audio.end", stream=self.stream))
        except asyncio.CancelledError:
            pass
        except Exception as exc:  # connection dropped mid-stream
            log.debug("audio pump stopped: %s", exc)
        finally:
            self.done.set()


class DeviceSession:
    """One authenticated device connection."""

    def __init__(self, hub: "DeviceHub", ws, hello: dict):
        self.hub = hub
        self._ws = ws
        self.session_id = uuid.uuid4().hex[:12]
        self.device_id: str = hello["device_id"]
        self.name: str = str(hello.get("name") or self.device_id)[:64]
        self.board: str = str(hello.get("board") or "unknown")[:64]
        self.firmware: str = str(hello.get("firmware") or "")[:32]
        caps = hello.get("caps")
        self.caps: dict = caps if isinstance(caps, dict) else {}
        self.actions: list[dict] = [
            a for a in (hello.get("actions") or [])
            if isinstance(a, dict) and isinstance(a.get("name"), str) and a["name"]
        ]
        sensors = hello.get("sensors")
        self.sensors: dict = dict(sensors) if isinstance(sensors, dict) else {}
        self.sensors_at: float = time.time() if self.sensors else 0.0
        self.paired = False
        self.connected_at = time.time()
        self.last_rx = time.monotonic()
        self.closed = False
        self._send_lock = asyncio.Lock()
        self._upload: _Upload | None = None
        self._audio_out: AudioOut | None = None
        self._stream_counter = 0
        self._pending: dict[str, asyncio.Future] = {}
        self._tasks: set[asyncio.Task] = set()
        self._ota_inbox: asyncio.Queue | None = None  # the device's ota.* replies, while an update runs

    # -- capabilities -----------------------------------------------------------

    @property
    def display(self) -> dict:
        d = self.caps.get("display")
        return d if isinstance(d, dict) else {}

    @property
    def has_speaker(self) -> bool:
        return isinstance(self.caps.get("speaker"), dict)

    @property
    def speaker_rate(self) -> int:
        spk = self.caps.get("speaker") or {}
        return int(spk.get("rate") or 16000) if isinstance(spk, dict) else 16000

    @property
    def charset(self) -> str:
        return str(self.display.get("charset") or "ascii")

    @property
    def image_box(self) -> tuple[int, int] | None:
        img = self.display.get("image")
        if not isinstance(img, dict) or img.get("format") != "rgb565":
            return None
        w, h = int(img.get("width") or 0), int(img.get("height") or 0)
        return (w, h) if w > 0 and h > 0 else None

    def action_names(self) -> list[str]:
        return [a["name"] for a in self.actions]

    # -- outbound -----------------------------------------------------------------

    async def send_json(self, obj: dict) -> None:
        payload = json.dumps({k: v for k, v in obj.items() if v is not None}, separators=(",", ":"))
        async with self._send_lock:
            await self._ws.send(payload)

    async def send_binary(self, data: bytes) -> None:
        async with self._send_lock:
            await self._ws.send(data)

    async def send_reply(self, text: str, *, turn: str | None = None, interim: bool = False) -> None:
        await self.send_json(protocol.message("reply", text=text, turn=turn, interim=interim or None))

    async def send_delta(self, text: str, *, turn: str | None = None) -> None:
        await self.send_json(protocol.message("reply.delta", text=text, turn=turn))

    async def send_transcript(self, text: str) -> None:
        await self.send_json(protocol.message("transcript", text=text))

    async def send_status(self, text: str) -> None:
        await self.send_json(protocol.message("status", text=text))

    async def send_notice(self, text: str, ttl_s: float = 8) -> None:
        await self.send_json(protocol.message("notice", text=text, ttl_s=ttl_s))

    async def turn_start(self, turn: str) -> None:
        await self.send_json(protocol.message("turn.start", turn=turn))

    async def turn_end(self, turn: str, outcome: str = "success") -> None:
        await self.send_json(protocol.message("turn.end", turn=turn, outcome=outcome))

    async def send_pairing(self, code: str, command: str) -> None:
        await self.send_json(protocol.message("pairing", code=code, command=command))

    async def set_paired(self, paired: bool) -> None:
        if paired == self.paired:
            return
        self.paired = paired
        if paired:
            self.hub.store.confirm(self.device_id)
        await self.send_json(protocol.message("paired" if paired else "unpaired"))

    async def ask(self, prompt_id: str, title: str, text: str, ttl_s: float | None = None) -> None:
        """Show a yes/no question; the answer arrives as ``on_prompt_reply``."""
        await self.send_json(protocol.message("prompt", id=prompt_id, title=title, text=text, ttl_s=ttl_s))

    async def close_prompt(self, prompt_id: str) -> None:
        await self.send_json(protocol.message("prompt.close", id=prompt_id))

    async def show_card(self, title: str, body: str, ttl_s: float = 15) -> None:
        await self.send_json(protocol.message("display", title=title, body=body, ttl_s=ttl_s))

    async def show_image(self, width: int, height: int, rgb565: bytes, ttl_s: float = 30) -> None:
        stream = self._next_stream()
        await self.send_json(protocol.message(
            "image.start", stream=stream, width=width, height=height, format="rgb565", ttl_s=ttl_s))
        for seq, off in enumerate(range(0, len(rgb565), IMAGE_CHUNK_BYTES)):
            await self.send_binary(protocol.binary(
                protocol.CHANNEL_IMAGE, stream, seq, rgb565[off: off + IMAGE_CHUNK_BYTES]))
        await self.send_json(protocol.message("image.end", stream=stream))

    def _next_stream(self) -> int:
        self._stream_counter = self._stream_counter % 250 + 1
        return self._stream_counter

    async def open_audio(self, src_rate: int, channels: int = 1, *, turn: str | None = None) -> AudioOut:
        """Start a new outbound audio stream, replacing any that is playing."""
        if self._audio_out and not self._audio_out.done.is_set():
            await self._audio_out.abort()
        out = AudioOut(self, self._next_stream(), src_rate, channels)
        self._audio_out = out
        await out.start(turn=turn)
        return out

    async def play_pcm(self, pcm: bytes, rate: int, *, turn: str | None = None) -> AudioOut:
        out = await self.open_audio(rate, turn=turn)
        out.write(pcm)
        out.finish()
        return out

    async def stop_audio(self) -> None:
        if self._audio_out:
            await self._audio_out.abort()

    async def invoke_action(self, name: str, args: dict | None = None, timeout: float = 10.0) -> dict:
        if name not in self.action_names():
            raise ActionError(f"{self.name} has no action {name!r}; available: {', '.join(self.action_names()) or 'none'}")
        action_id = uuid.uuid4().hex[:12]
        fut = asyncio.get_running_loop().create_future()
        self._pending[action_id] = fut
        try:
            await self.send_json(protocol.message("action", id=action_id, name=name, args=args or {}))
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            raise ActionError(f"{self.name} did not answer {name!r} within {timeout:.0f}s") from None
        finally:
            self._pending.pop(action_id, None)

    async def update_firmware(self, image, progress=None) -> str:
        """Install a firmware image (``firmware.bin`` bytes or an ``ota.FirmwareImage``).

        Returns the version the device installed; it then restarts and reconnects.
        Raises ``ota.UpdateError`` when the image doesn't suit the device or the device refuses it.
        """
        from . import ota

        img = image if isinstance(image, ota.FirmwareImage) else ota.inspect_image(image)
        ota.check_for(img, self)
        key = self.hub.store.key_for(self.device_id)
        if key is None:
            raise ota.UpdateError("not_enrolled", f"{self.device_id} has no enrolled key")
        return await ota.send_update(self, img, key, progress=progress)

    async def close(self, reason: str = "") -> None:
        try:
            await self._ws.close(1000, reason[:100])
        except Exception:
            pass

    def spawn(self, coro) -> asyncio.Task:
        """Run work for this device; :meth:`DeviceHub.stop` cancels whatever is still running."""
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        self.hub._tasks.add(task)
        task.add_done_callback(self._task_done)
        return task

    def _task_done(self, task: asyncio.Task) -> None:
        self._tasks.discard(task)
        self.hub._tasks.discard(task)
        if not task.cancelled() and task.exception() is not None:
            log.warning("[%s] handler failed: %s", self.device_id, task.exception())

    def _shutdown(self) -> None:
        self.closed = True
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(ActionError(f"{self.name} disconnected"))
        if self._ota_inbox is not None:
            self._ota_inbox.put_nowait(
                protocol.message("ota.error", code="disconnected", message=f"{self.name} disconnected"))
        if self._audio_out and self._audio_out._task:
            self._audio_out._task.cancel()

    # -- description ----------------------------------------------------------------

    def describe(self) -> dict:
        disp = self.display
        return {
            "device_id": self.device_id,
            "name": self.name,
            "board": self.board,
            "firmware": self.firmware,
            "paired": self.paired,
            "connected_for_s": int(time.time() - self.connected_at),
            "screen": {k: disp[k] for k in ("width", "height", "text_cols", "text_rows") if k in disp} or None,
            "speaker": self.has_speaker,
            "microphone": isinstance(self.caps.get("mic"), dict),
            "inputs": self.caps.get("inputs") or [],
            "actions": [
                {"name": a["name"], "description": a.get("description", ""), "params": a.get("params") or {}}
                for a in self.actions
            ],
            "sensors": self.sensors,
            "sensors_age_s": int(time.time() - self.sensors_at) if self.sensors_at else None,
        }

    def prompt_context(self) -> str:
        """Stable per-device context for the model (only changes if the device does)."""
        parts = [f'The user is speaking through the gadget "{self.name}" (board {self.board}, id {self.device_id}).']
        disp = self.display
        if disp.get("text_cols") and disp.get("text_rows"):
            parts.append(
                f"Its screen shows about {disp['text_rows']} lines of {disp['text_cols']} characters; "
                "longer replies must be scrolled.")
        parts.append("Replies are also spoken aloud by its speaker." if self.has_speaker
                     else "It has no speaker; replies are only shown on screen.")
        if self.actions:
            names = ", ".join(sorted(self.action_names()))
            parts.append(f"Device actions you can invoke with the gadget_action tool: {names}.")
        return " ".join(parts)


class DeviceHub:
    def __init__(
        self,
        store: DeviceStore,
        delegate: HubDelegate,
        *,
        host: str = "0.0.0.0",
        port: int = 8765,
        path: str = "/gadget",
        access_token: str | None = None,
        heartbeat_s: int = 20,
        ssl_context=None,
        max_utterance_s: float = 60.0,
    ):
        self.store = store
        self.delegate = delegate
        self.host, self.port, self.path = host, port, "/" + path.strip("/")
        self.access_token = access_token or None
        self.heartbeat_s = max(5, int(heartbeat_s))
        self.ssl_context = ssl_context
        self.max_utterance_s = max_utterance_s
        self.sessions: dict[str, DeviceSession] = {}
        self.loop: asyncio.AbstractEventLoop | None = None
        self._server = None
        self._tasks: set[asyncio.Task] = set()  # DeviceSession.spawn, including devices already gone

    # -- lifecycle -------------------------------------------------------------------

    async def start(self) -> None:
        from websockets.asyncio.server import serve

        self.loop = asyncio.get_running_loop()
        self._server = await serve(
            self._handle, self.host, self.port,
            subprotocols=[protocol.SUBPROTOCOL],
            process_request=self._check_request,
            ping_interval=None,  # the protocol has its own heartbeat the firmware can see
            close_timeout=3,
            max_size=MAX_FRAME_BYTES,
            ssl=self.ssl_context,
        )
        log.info("gadget hub listening on %s://%s:%s%s", "wss" if self.ssl_context else "ws",
                 self.host, self.bound_port, self.path)

    @property
    def bound_port(self) -> int:
        if self._server and self._server.sockets:
            return int(self._server.sockets[0].getsockname()[1])
        return self.port

    @property
    def running(self) -> bool:
        return self._server is not None

    async def stop(self) -> None:
        await asyncio.gather(*(s.close("server stopping") for s in list(self.sessions.values())),
                             return_exceptions=True)
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        # A reply still streaming or a reminder still waiting ends with the server.
        tasks = [task for task in self._tasks if task is not asyncio.current_task()]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    def get(self, device_id: str) -> DeviceSession | None:
        return self.sessions.get(device_id)

    def find(self, ref: str | None) -> DeviceSession | None:
        """Look a device up by id or (case-insensitive) name."""
        if not ref:
            return None
        if ref in self.sessions:
            return self.sessions[ref]
        wanted = ref.strip().lower()
        matches = [s for s in self.sessions.values() if s.name.lower() == wanted]
        return matches[0] if len(matches) == 1 else None

    # -- connection handling -----------------------------------------------------------

    def _check_request(self, connection, request):
        if request.path.split("?", 1)[0].rstrip("/") != self.path.rstrip("/"):
            return connection.respond(404, "Not a Hermes gadget endpoint\n")
        return None

    @staticmethod
    def _address(ws) -> str:
        try:
            return str(ws.remote_address[0])
        except (AttributeError, IndexError, TypeError):
            return ""

    async def _recv_json(self, ws, timeout: float) -> dict:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout)
        except asyncio.TimeoutError:
            raise Rejected("timeout", "handshake timed out") from None
        if isinstance(raw, bytes):
            raise Rejected("protocol", "expected a JSON frame during the handshake")
        try:
            msg = json.loads(raw)
        except ValueError:
            raise Rejected("protocol", "malformed JSON") from None
        if not isinstance(msg, dict):
            raise Rejected("protocol", "expected a JSON object")
        return msg

    async def _authenticate(self, ws) -> dict:
        hello = await self._recv_json(ws, HANDSHAKE_TIMEOUT_S)
        if hello.get("type") != "hello":
            raise Rejected("protocol", "expected hello")
        if hello.get("proto") != protocol.VERSION:
            raise Rejected("protocol", f"unsupported protocol version {hello.get('proto')!r}; server speaks {protocol.VERSION}")
        device_id = hello.get("device_id")
        if not isinstance(device_id, str) or not protocol.DEVICE_ID_RE.match(device_id):
            raise Rejected("protocol", "invalid device_id")
        if self.access_token and not secrets.compare_digest(str(hello.get("token") or ""), self.access_token):
            raise Rejected("bad_token", "access token missing or wrong")

        nonce = base64.b64encode(secrets.token_bytes(16)).decode()
        known = self.store.key_for(device_id)
        await ws.send(json.dumps(protocol.message("challenge", nonce=nonce, enrolled=known is not None)))
        auth = await self._recv_json(ws, HANDSHAKE_TIMEOUT_S)
        if auth.get("type") != "auth":
            raise Rejected("protocol", "expected auth")
        if known is not None:
            if not protocol.verify_mac(known, device_id, nonce, str(auth.get("mac") or "")):
                raise Rejected(
                    "auth_failed",
                    f"device key does not match the enrolled key; on the Hermes host run "
                    f"'hermes gadget forget {device_id}' to re-enroll")
        else:
            key = protocol.decode_key(str(auth.get("key") or ""))
            if key is None or protocol.device_id_for_key(key) != device_id:
                raise Rejected("auth_failed", "enrollment key does not match device_id")
            # Anyone who reaches the port can enroll; pairing is what admits them. Until
            # then a device is pending, and only so many may wait at once.
            address = self._address(ws)
            pending = self.store.pending()
            if len(pending) >= MAX_PENDING_DEVICES:
                raise Rejected("busy", f"{len(pending)} devices are already waiting to be paired; approve or "
                                       "forget some first (hermes gadget devices)")
            if sum(1 for rec in pending.values() if rec.get("address") == address) >= MAX_PENDING_PER_ADDRESS:
                raise Rejected("busy", f"{address} already has {MAX_PENDING_PER_ADDRESS} devices waiting to be paired")
            self.store.enroll(device_id, key, name=str(hello.get("name") or ""), board=str(hello.get("board") or ""),
                              address=address)
            log.info("enrolled new device %s (%s) from %s", device_id, hello.get("name"), address)
        self.store.touch(device_id, name=str(hello.get("name") or ""), board=str(hello.get("board") or ""),
                         firmware=str(hello.get("firmware") or "")[:32])
        return hello

    async def _handle(self, ws) -> None:
        session: DeviceSession | None = None
        heartbeat: asyncio.Task | None = None
        try:
            try:
                hello = await self._authenticate(ws)
            except Rejected as r:
                log.warning("rejected device connection: %s", r.message)
                await ws.send(json.dumps(protocol.message("error", code=r.code, message=r.message)))
                await ws.close(1008, r.code)
                return

            session = DeviceSession(self, ws, hello)
            previous = self.sessions.get(session.device_id)
            self.sessions[session.device_id] = session
            if previous is not None:
                await previous.close("replaced by a newer connection")
            session.paired = bool(await self.delegate.is_paired(session))
            if session.paired:
                self.store.confirm(session.device_id)
            await session.send_json(protocol.message(
                "welcome", session=session.session_id, paired=session.paired,
                heartbeat_s=self.heartbeat_s, server="hermes", proto=protocol.VERSION))
            log.info("device %s (%s) online, paired=%s", session.device_id, session.name, session.paired)
            await self.delegate.on_ready(session)
            heartbeat = asyncio.create_task(self._heartbeat(session))

            async for raw in ws:
                session.last_rx = time.monotonic()
                if isinstance(raw, bytes):
                    self._on_binary(session, raw)
                else:
                    await self._on_text(session, raw)
        except Exception as exc:
            from websockets.exceptions import ConnectionClosed

            if not isinstance(exc, ConnectionClosed):
                log.exception("device connection failed: %s", exc)
        finally:
            if heartbeat:
                heartbeat.cancel()
            if session is not None:
                session._shutdown()
                if self.sessions.get(session.device_id) is session:
                    del self.sessions[session.device_id]
                log.info("device %s offline", session.device_id)
                try:
                    await self.delegate.on_disconnect(session)
                except Exception:
                    log.exception("on_disconnect failed")

    async def _heartbeat(self, session: DeviceSession) -> None:
        while True:
            await asyncio.sleep(self.heartbeat_s)
            if time.monotonic() - session.last_rx > 3 * self.heartbeat_s:
                log.info("device %s silent; closing", session.device_id)
                await session.close("heartbeat timeout")
                return
            try:
                await session.send_json(protocol.message("ping", ts=int(time.time() * 1000)))
            except Exception:
                return

    # -- inbound ---------------------------------------------------------------------------

    async def _on_text(self, session: DeviceSession, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except ValueError:
            log.debug("[%s] malformed frame", session.device_id)
            return
        if not isinstance(msg, dict):
            return
        handler = self._routes.get(str(msg.get("type")))
        if handler is None:
            log.debug("[%s] ignoring %r", session.device_id, msg.get("type"))
            return
        await handler(self, session, msg)

    async def _h_text(self, session: DeviceSession, msg: dict) -> None:
        text = str(msg.get("text") or "").strip()
        if text:
            session.spawn(self.delegate.on_text(session, str(msg.get("id") or ""), text[:4000]))

    async def _h_audio_start(self, session: DeviceSession, msg: dict) -> None:
        fmt = msg.get("format", "pcm16")
        if fmt != "pcm16":
            await session.send_notice(f"Unsupported audio format {fmt!r}")
            return
        rate = int(msg.get("rate") or 16000)
        if not 4000 <= rate <= 48000:
            await session.send_notice(f"Unsupported sample rate {rate}")
            return
        session._upload = _Upload(
            id=str(msg.get("id") or ""), stream=int(msg.get("stream") or 0), rate=rate, started=time.monotonic())

    def _on_binary(self, session: DeviceSession, data: bytes) -> None:
        frame = protocol.parse_binary(data)
        up = session._upload
        if frame is None or frame.channel != protocol.CHANNEL_AUDIO or up is None or frame.stream != up.stream:
            return
        if frame.seq != up.next_seq & 0xFFFF:
            up.gaps += 1
        up.next_seq = frame.seq + 1
        limit = int(self.max_utterance_s * up.rate * 2)
        room = limit - len(up.pcm)
        if room <= 0:
            up.truncated = True
            return
        up.pcm += frame.payload[:room]

    async def _h_audio_end(self, session: DeviceSession, msg: dict) -> None:
        up = session._upload
        session._upload = None
        if up is None or int(msg.get("stream") or 0) != up.stream:
            return
        seconds = audio.duration_s(bytes(up.pcm), up.rate)
        if up.gaps:
            log.debug("[%s] utterance had %d sequence gaps", session.device_id, up.gaps)
        if seconds < MIN_UTTERANCE_S:
            await session.send_notice("Didn't catch that - hold the button while speaking")
            return
        wav = audio.wav_bytes(bytes(up.pcm), up.rate)
        session.spawn(self.delegate.on_utterance(session, up.id, wav, seconds))

    async def _h_audio_cancel(self, session: DeviceSession, msg: dict) -> None:
        session._upload = None

    async def _h_cancel(self, session: DeviceSession, msg: dict) -> None:
        await session.stop_audio()
        session.spawn(self.delegate.on_cancel(session))

    async def _h_session_new(self, session: DeviceSession, msg: dict) -> None:
        await session.stop_audio()
        session.spawn(self.delegate.on_new_session(session))

    async def _h_prompt_reply(self, session: DeviceSession, msg: dict) -> None:
        prompt_id = str(msg.get("id") or "")[:64]
        answer = msg.get("answer")
        if prompt_id and answer in ("yes", "no"):
            session.spawn(self.delegate.on_prompt_reply(session, prompt_id, answer == "yes"))

    async def _h_action_result(self, session: DeviceSession, msg: dict) -> None:
        fut = session._pending.get(str(msg.get("id") or ""))
        if fut is None or fut.done():
            return
        if msg.get("ok"):
            fut.set_result(msg.get("result") if isinstance(msg.get("result"), dict) else {"value": msg.get("result")})
        else:
            fut.set_exception(ActionError(str(msg.get("error") or "action failed")))

    async def _h_state(self, session: DeviceSession, msg: dict) -> None:
        sensors = msg.get("sensors")
        if isinstance(sensors, dict):
            session.sensors.update(sensors)
            session.sensors_at = time.time()
            await self.delegate.on_state(session, dict(session.sensors))

    async def _h_event(self, session: DeviceSession, msg: dict) -> None:
        name = str(msg.get("name") or "").strip()[:64]
        if name:
            session.spawn(self.delegate.on_event(session, name, msg.get("data"), bool(msg.get("notify"))))

    async def _h_ping(self, session: DeviceSession, msg: dict) -> None:
        await session.send_json(protocol.message("pong", ts=msg.get("ts")))

    async def _h_pong(self, session: DeviceSession, msg: dict) -> None:
        pass  # last_rx already refreshed

    async def _h_ota(self, session: DeviceSession, msg: dict) -> None:
        if session._ota_inbox is not None:
            session._ota_inbox.put_nowait(msg)

    _routes = {
        "text": _h_text,
        "audio.start": _h_audio_start,
        "audio.end": _h_audio_end,
        "audio.cancel": _h_audio_cancel,
        "cancel": _h_cancel,
        "session.new": _h_session_new,
        "prompt.reply": _h_prompt_reply,
        "action.result": _h_action_result,
        "state": _h_state,
        "event": _h_event,
        "ping": _h_ping,
        "pong": _h_pong,
        "ota.ready": _h_ota,
        "ota.ack": _h_ota,
        "ota.done": _h_ota,
        "ota.error": _h_ota,
    }
