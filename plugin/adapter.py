"""Hermes gateway platform adapter for Hermes Gadget devices.

Each connected device is a direct-message chat whose ``chat_id`` and
``user_id`` are the device id. Everything Hermes already does for messaging
platforms applies unchanged: voice notes are transcribed by the gateway's STT,
replies are spoken through auto-TTS (streaming PCM when the TTS provider
supports it), unknown devices go through Hermes's DM pairing codes, and
``/stop`` interrupts a running turn. Confirmations (``/new``, dangerous
commands) appear on the device as yes/no questions answered with its buttons.
A device that supports it chooses which Hermes profile answers it; the adapter
stamps that profile on every message, and a multiplexed gateway runs the turn there.

config.yaml::

    platforms:
      gadget:
        enabled: true
        extra:
          host: 0.0.0.0          # interface devices connect to
          port: 8765
          path: /gadget
          speak_replies: true    # auto-TTS for devices with a speaker
          auto_home: true        # first approved device becomes the gadget home channel
          unauthorized_dm_behavior: pair
          dashboard: true        # sessions and status tiles on devices that draw them
          profiles: [work, home] # optional: the profiles devices may switch to (default always)
          profile_faces: true    # each profile's face (gadget-face.bin or its avatar) on devices
          stt_language: auto     # or a language code such as pt: the language gadget speech is transcribed in

Secrets (``~/.hermes/.env``): ``GADGET_ACCESS_TOKEN`` (optional shared token).
"""

from __future__ import annotations

import asyncio
import contextlib
import inspect
import json
import logging
import os
import re
import ssl
import tempfile
import time
import uuid
import weakref
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from agent.i18n import get_language, t
from gateway.config import Platform, PlatformConfig
from gateway.platforms._shared import extra_or_secret, get_scoped_secret as _get_scoped_secret
from gateway.platforms.base import (
    AudioFormat,
    BasePlatformAdapter,
    ExecApprovalPrompt,
    SendResult,
    StreamingTTSHandle,
    cache_audio_from_bytes_async,
    cache_image_from_url,
)
from gateway.platforms.event import MessageEvent, MessageType

from . import audio as gaudio
from . import botchat, compat, dashboard, imaging, interpret, ota, runtime, stt, textfmt
from . import diag as gdiag
from .desk import Desk
from . import status as live_status
from . import face as faces
from .battery_log import BatteryLog
from .hub import DeviceHub, DeviceSession, HubDelegate
from .store import DeviceStore

logger = logging.getLogger(__name__)

PLATFORM_NAME = "gadget"
MAX_MESSAGE_LENGTH = 4000
VIRTUAL_SEP = "~"             # <device>~<profile>: a device's chat with a non-default profile (profile_mode: virtual)
PAIRING_TTL_S = 3600          # mirrors Hermes's pairing-code lifetime
PAIRING_POLL_S = 2.0
# A profile's changed face or avatar files are read for its thumbnail once they have held still this long.
THUMB_HOLD_S = 1.5
PAIRING_GRACE_S = 6.0
UPDATE_POLL_S = 3.0           # how often staged firmware ('hermes gadget update') is looked for
DASHBOARD_POLL_S = 2.0        # how often tiles ('hermes gadget tile') and expiries are looked at
DASHBOARD_SETTLE_S = 0.3      # session changes this close together go out as one dashboard
DESK_POLL_S = 0.4             # how often the Desktop page's request inbox is checked
STATUS_POLL_S = 0.5           # how often live device state is compared with status.json (cheap: a compare)
# Trigger text for Hermes's unauthorized-DM flow. Never reaches the agent while the
# device is unpaired; if approval races it, it is a cheap read-only command.
PAIRING_TRIGGER = "/status"
HOST_ONLY_TEXT = "Can't show all of it here. Approve or deny it on the host."
_PAIRING_CMD = re.compile(r"(hermes\s+(?:-p\s+\S+\s+)?pairing\s+approve\s+\S+\s+([A-Z2-9]{8}))\b")
# What Hermes wraps around cron output (cron/scheduler_delivery.py), and the note it appends when
# output is cut to fit a platform (gateway/delivery.py). Neither is worth reading aloud.
_CRON_WRAP = re.compile(r"^Cronjob Response: .*?\n-{5,}\n\n(.*?)(?:\n\nTo stop or manage this job.*)?$", re.S)
_TRUNCATED_NOTE = re.compile(r"\n\n\.\.\. \[truncated, full output saved to [^\]]*\]\s*$")
# A message that arrives while the device is busy (a turn, or audio already playing) waits this long
# for it to finish; after that it stays text only.
SPEECH_QUEUE_WAIT_S = 120.0
_NOT_SPOKEN = "_gadget_not_spoken"  # metadata: this text is played or spoken another way (a caption, a question)
# The gateway echoes speech-to-text results with this translated line: a microphone emoji and the
# quoted transcript, quoted per language (Ukrainian uses «»). Patterns are built from the line itself.
_ECHO_KEY = "gateway.voice.transcript_echo_short"
_ECHO_SLOT = "\x00"
_echo_patterns: Dict[str, Optional[re.Pattern]] = {}
# Holding CANCEL already is a deliberate "start over", so the /new it sends confirms itself.
AUTO_CONFIRM_NEW_S = 15.0
# One translation. Hermes may retry a failed request, also on a fallback provider, each try within
# INTERPRET_LLM_TIMEOUT_S; the deadlines bound the whole of it, and the speech-to-text before it.
INTERPRET_LLM_TIMEOUT_S = 20.0
INTERPRET_LLM_DEADLINE_S = 45.0
INTERPRET_STT_DEADLINE_S = 60.0
INTERPRET_MAX_TOKENS = 512  # a translation of one recording; not an essay to read aloud
MAX_HELD_PUSHES = 20  # messages kept for a gadget until it leaves interpreter mode
PROMPT_PREFIX = "prompt:"  # message ids of device questions, so edits can withdraw them
SLASH_CONFIRM_TTL_S = 300  # mirrors tools.slash_confirm.DEFAULT_TIMEOUT_SECONDS
# Hermes words confirmations for chat apps: a bold header and the detail, then a
# list of choices and an italic note on typed replies. A device keeps the first two.
_CHOICE_LIST = re.compile(r"^\s*[•*-]\s", re.MULTILINE)
_ITALIC_NOTE = re.compile(r"^_.*_$", re.DOTALL)


def _flag(value: Any, default: bool) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def _discard_speech(job: "asyncio.Future") -> None:
    """Delete what a text-to-speech call wrote when nobody waited for it any more."""
    with contextlib.suppress(Exception):
        result = json.loads(job.result() or "{}")
        for path in (result.get("file_paths") or [result.get("file_path")]):
            if path:
                with contextlib.suppress(OSError):
                    os.unlink(str(path))


@dataclass
class GadgetTTSHandle(StreamingTTSHandle):
    out: Any = None


@dataclass
class _Prompt:
    """A question shown on a device, and how to hand its answer back to Hermes."""
    id: str
    kind: str  # "slash" (tools.slash_confirm) or "approval" (tools.approval)
    session_key: str
    title: str
    text: str
    confirm_id: str = ""
    ttl_s: Optional[float] = None
    host_only: bool = False  # the device can't show the whole command, so its YES is refused
    created: float = field(default_factory=time.monotonic)

    def expired(self) -> bool:
        return self.ttl_s is not None and time.monotonic() - self.created > self.ttl_s


@dataclass
class _FaceLook:
    """A connection's last face lookup, and what the pairing check has seen of its files since (faces.stamp)."""
    profile: Optional[str]  # the profile it could show; None for the mascot
    seen: Optional[tuple] = None  # the files as the lookup read them; None until stat'ed
    last: Optional[tuple] = None  # the files at the last pairing check
    settled: Optional[tuple] = None  # files that held still for two checks, for the next lookup to read


# A lookup that found the files changed after they settled: never sent, so the device keeps its face.
_KEEP_FACE = faces.ProfileFace("", bytes, valid=lambda: False)


def spoken_question(question: str, choices: Optional[list]) -> str:
    """A clarify question as it is read aloud: the question, then its choices by number."""
    text = (question or "").strip()
    picks = [str(c).strip() for c in choices or [] if str(c).strip()]
    if picks:
        listed = " ".join(f"{i}: {c.rstrip('.')}." for i, c in enumerate(picks, start=1))
        text = f"{text} The choices are {listed} Say the number or your own answer."
    return text


def spoken_message(content: str, metadata: Optional[Dict[str, Any]] = None) -> str:
    """A message sent outside a turn as it is read aloud: no Markdown, and for cron output
    (``job_id`` in the metadata) no header, footer or truncation note."""
    text = content or ""
    if (metadata or {}).get("job_id"):
        text = _TRUNCATED_NOTE.sub("", text)
        if m := _CRON_WRAP.match(text):
            text = m.group(1)
    return textfmt.strip_markdown(text).strip()


def transcript_echo(content: str) -> Optional[str]:
    """The transcript, if ``content`` is the gateway's echo line in Hermes's active language."""
    line = t(_ECHO_KEY, text=_ECHO_SLOT)
    if line not in _echo_patterns:
        head, slot, tail = line.partition(_ECHO_SLOT)
        head, tail = head.strip(), tail.strip()
        _echo_patterns[line] = (re.compile(rf"^\s*{re.escape(head)}\s*(.*?)\s*{re.escape(tail)}\s*$", re.DOTALL)
                                if slot and (head or tail) else None)
    match = _echo_patterns[line].match(content) if _echo_patterns[line] else None
    return match.group(1) if match else None


def shown_command(command: str, display: dict) -> Optional[str]:
    """The command exactly as the device will draw it, or None if the device can't show all of it.

    Approval text is never reshaped: stripping Markdown would drop ``<think>`` blocks, links and
    pipes, so the card could show less than what runs. The device draws printable ASCII, wraps at
    spaces (collapsing runs of them) and cuts a question to fit beside its mascot, down to one
    line on the smallest screens, so only a command that fits one line as-is is shown. That line
    keeps two columns free: when the description below it is cut, the device writes ".." over
    the end of the last line it keeps.
    """
    command = (command or "").strip()
    cols = display.get("text_cols")
    if not command or not isinstance(cols, int) or len(command) > cols - 2 or "  " in command:
        return None
    if any(not " " <= ch <= "~" for ch in command):  # newlines, tabs, controls, anything non-ASCII
        return None
    return command


def confirm_text(title: str, message: str, charset: str = "ascii") -> Tuple[str, str]:
    """Reduce a Hermes confirmation to a device question: (headline, detail)."""
    paragraphs = [p.strip() for p in re.split(r"\n\s*\n", message or "") if p.strip()]
    kept = [p for p in paragraphs if not _CHOICE_LIST.search(p) and not _ITALIC_NOTE.match(p)]
    head = textfmt.for_device(kept.pop(0), charset) if kept else ""
    detail = textfmt.for_device(" ".join(kept), charset)
    return head or textfmt.for_device(title, charset), detail


def _profile_label(info: Any) -> str:
    """A profile's name as the Hermes interface shows it: its display name, else its Desktop Bots
    title (``profile.yaml`` ``ui_meta['hermes-bots'].title``, which Hermes reads as ``bot_title``), else its id
    with a capital first letter."""
    for attr in ("display_name", "bot_title"):
        label = str(getattr(info, attr, "") or "").strip()
        if label:
            return label
    name = str(info.name)
    return name[:1].upper() + name[1:]


class GadgetAdapter(BasePlatformAdapter, HubDelegate):
    MAX_MESSAGE_LENGTH = MAX_MESSAGE_LENGTH
    supports_status_text = True

    def __init__(self, config: PlatformConfig):
        super().__init__(config=config, platform=Platform(PLATFORM_NAME))
        gdiag.keep_host_log()  # the hub's side of a diagnostics report (desk "diag" requests)
        extra = config.extra or {}
        self._host = str(extra.get("host") or "0.0.0.0")
        port = extra.get("port")
        self._port = 8765 if port in (None, "") else int(port)  # 0 = any free port
        self._path = str(extra.get("path") or "/gadget")
        self._heartbeat_s = int(extra.get("heartbeat_s") or 20)
        self._max_utterance_s = float(extra.get("max_utterance_s") or 60)
        # Speaker audio as Opus at this bitrate for devices that decode it; 0 keeps PCM16.
        opus_bitrate = extra.get("opus_bitrate")
        self._opus_bitrate = 24000 if opus_bitrate in (None, "") else int(opus_bitrate)
        self._speak_replies = _flag(extra.get("speak_replies"), True)
        self._auto_home = _flag(extra.get("auto_home"), True)
        self._battery_log_on = _flag(extra.get("battery_log"), False)
        self._dashboard_on = _flag(extra.get("dashboard"), True)
        self._profile_faces = _flag(extra.get("profile_faces"), True)
        self._convert_avatars: Optional[bool] = None  # Pillow present; asked once
        # Profile switching: off without the key; "all" offers every served profile; a list offers those.
        offered = extra.get("profiles")
        self._offer_all = isinstance(offered, str) and offered.strip().lower() == "all"
        self._offered: Optional[set] = None if offered is None or self._offer_all else {
            str(p).strip() for p in (offered if isinstance(offered, list) else []) if str(p).strip()}
        # "virtual": a device talking to another profile becomes its own chat, ``<device>~<profile>``, that a
        # ``gateway.profile_routes`` entry the plugin adds at run time sends to that profile. Hermes then
        # admits it like any routed chat, so no core hook is needed, and each profile keeps its own sessions.
        self._virtual_profiles = str(extra.get("profile_mode") or "").strip().lower() == "virtual"
        self._virtual_routes: set = set()  # virtual chat ids that resolved to their profile's route
        self._virtual_owned: dict = {}     # chat id -> the ProfileRoute this adapter added (and may remove)
        # The language `welcome` names for devices left on auto: Hermes's own unless set here.
        self._language = str(extra.get("language") or "auto").strip().lower() or "auto"
        # The language gadget recordings are transcribed in, whichever profile answers; None = its STT settings.
        self._stt_language = stt.normalize(extra.get("stt_language"))
        self._tiles: Optional[dashboard.TileStore] = None
        self._dashboard_task: Optional[asyncio.Task] = None
        self._dashboard_wake: Optional[asyncio.Event] = None
        self._boards: Dict[dashboard.SessionBoard, Callable[[], None]] = {}  # each board read, its unsubscribe
        self._roster_lock = asyncio.Lock()  # one roster read at a time; never held across a send
        self._speech_locks: Dict[str, asyncio.Lock] = {}  # device -> one out-of-turn message spoken at a time
        self._push_seq = 0
        self._pushed_seq: Dict[str, int] = {}  # device -> the newest push that read a roster for it
        # connection -> its last face lookup; it goes with the connection, and a replaced one keeps its own
        self._face_from: "weakref.WeakKeyDictionary[DeviceSession, _FaceLook]" = weakref.WeakKeyDictionary()
        # Profile thumbnails (thumb()): (profile, size, avatars on) -> (the files' stamp, the thumbnail), and
        # a stamp seen changing that has yet to hold still, with when it was first seen.
        self._thumbs: Dict[tuple, tuple] = {}
        self._thumb_changing: Dict[tuple, tuple] = {}
        self._battery_log: Optional[BatteryLog] = None
        self._tls_cert = extra.get("tls_cert")
        self._tls_key = extra.get("tls_key")
        self._access_token = extra_or_secret(extra, "access_token", "GADGET_ACCESS_TOKEN") or None
        self._hub: Optional[DeviceHub] = None
        self._store: Optional[DeviceStore] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._turns: Dict[str, str] = {}
        # Interpreter mode (docs/hermes-integration.md#interpreter-mode): device id -> its languages and
        # who speaks next, and the translation under way.
        self._interpreting: Dict[str, interpret.Session] = {}
        self._interpret_store: Optional[interpret.Store] = None  # the same on disk (connect)
        self._interpret_tasks: Dict[str, asyncio.Task] = {}
        self._interpret_turns: Dict[str, str] = {}  # device id -> the translation's turn id
        self._held_pushes: Dict[str, List[tuple]] = {}  # device id -> (chat id, text, metadata) kept for later
        # device id -> the agent turn that started the mode from the gadget's own chat, whose own
        # short reply still reaches the gadget; no other turn's output does while the mode is on.
        self._interpret_asker: Dict[str, str] = {}
        self._llm: Any = None  # tests may stand in for Hermes's ctx.llm; None = this profile's own
        self._watch_task: Optional[asyncio.Task] = None
        self._update_task: Optional[asyncio.Task] = None
        self._status_task: Optional[asyncio.Task] = None
        self._desk_task: Optional[asyncio.Task] = None
        self._status_dir: Optional[Path] = None
        self._status_failing = False  # warned once; again only after a write succeeds
        self._installing: Dict[str, asyncio.Task] = {}  # device id -> staged update being installed
        self._registry_key = "default"
        self._prompts: Dict[str, List[_Prompt]] = {}  # device id -> questions, oldest (shown) first
        self._new_requested: Dict[str, float] = {}  # device id -> when it asked for a new session
        self._owner_check = None  # tests may stand in for the default profile's verdict; None = this adapter's own check
        self._routed_refused: set = set()  # owner-bound devices a route sends elsewhere, warned about once

    # -- lifecycle ------------------------------------------------------------------

    def _ssl_context(self) -> Optional[ssl.SSLContext]:
        if not self._tls_cert:
            return None
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self._tls_cert, self._tls_key or None)
        return ctx

    async def connect(self, *, is_reconnect: bool = False) -> bool:
        from plugins.plugin_storage import plugin_data_dir

        self._store = DeviceStore(plugin_data_dir(PLATFORM_NAME))
        # A gadget left interpreting may be in a stranger's hands: a restart must not forget it.
        self._interpret_store = interpret.Store(plugin_data_dir(PLATFORM_NAME))
        self._interpreting.update(await asyncio.to_thread(self._interpret_store.load))
        if self._battery_log_on:
            self._battery_log = BatteryLog(plugin_data_dir(PLATFORM_NAME) / "battery")
        try:
            ssl_ctx = self._ssl_context()
        except (OSError, ssl.SSLError) as exc:
            self._set_fatal_error("gadget_tls", f"Cannot load TLS certificate: {exc}", retryable=False)
            return False
        self._hub = DeviceHub(
            self._store, self, host=self._host, port=self._port, path=self._path,
            access_token=self._access_token, heartbeat_s=self._heartbeat_s, ssl_context=ssl_ctx,
            max_utterance_s=self._max_utterance_s, opus_bitrate=self._opus_bitrate)
        try:
            await self._hub.start()
        except OSError as exc:
            self._hub = None
            self._set_fatal_error(
                "gadget_listen_failed", f"Cannot listen on {self._host}:{self._port}: {exc}", retryable=True)
            return False
        self._loop = asyncio.get_running_loop()
        self._registry_key = getattr(self, "_owner_profile", None) or "default"
        runtime.register(self._registry_key, self)
        self._watch_task = asyncio.create_task(self._watch_pairing())
        self._update_task = asyncio.create_task(self._watch_updates())
        self._status_dir = plugin_data_dir(PLATFORM_NAME)
        self._status_task = asyncio.create_task(self._watch_status())
        self._desk_task = asyncio.create_task(self._watch_desk())
        if self._dashboard_on:
            self._tiles = dashboard.TileStore(plugin_data_dir(PLATFORM_NAME))
            self._dashboard_wake = asyncio.Event()
            self._boards[dashboard.BOARD] = dashboard.BOARD.subscribe(self._board_changed)
            self._dashboard_task = asyncio.create_task(self._watch_dashboard())
        self._mark_connected()
        self._wire_plugin_handlers(None)
        logger.info("[%s] devices connect to %s://<this host>:%s%s", self.name,
                    "wss" if ssl_ctx else "ws", self._hub.bound_port, self._hub.path)
        return True

    async def disconnect(self) -> None:
        runtime.unregister(self._registry_key, self)
        for unsubscribe in self._boards.values():
            unsubscribe()
        self._boards.clear()
        for task in (self._watch_task, self._update_task, self._dashboard_task, self._status_task, self._desk_task):
            if task:
                task.cancel()
        self._watch_task = self._update_task = self._dashboard_task = self._status_task = self._desk_task = None
        if self._hub:
            await self._hub.stop()
            self._hub = None
        if self._status_dir is not None:
            self._write_status({}, running=False)  # stopped on purpose: no device is online
        self._pushed_seq.clear()  # after the stop: a push still finishing while it ran may add one
        self._mark_disconnected()

    @property
    def hub(self) -> Optional[DeviceHub]:
        return self._hub

    @property
    def loop(self) -> Optional[asyncio.AbstractEventLoop]:
        return self._loop

    @staticmethod
    def _device_id(chat_id: str) -> str:
        """The device behind a chat id: itself, or the device part of a virtual ``<device>~<profile>``."""
        return str(chat_id).split(VIRTUAL_SEP, 1)[0]

    def _session(self, chat_id: str) -> Optional[DeviceSession]:
        return self._hub.get(self._device_id(chat_id)) if self._hub else None

    def _virtual_chat(self, device_id: str, profile: str) -> Optional[str]:
        """The virtual chat for a device's talk with a non-default profile, its route added if missing.
        None when the gateway's route list cannot be reached: the caller falls back to the stamp."""
        chat_id = f"{device_id}{VIRTUAL_SEP}{profile}"
        config = getattr(getattr(self, "gateway_runner", None), "config", None)
        routes = getattr(config, "profile_routes", None)
        if routes is None:
            return None
        try:
            from gateway.profile_routing import ProfileRoute
        except ImportError:
            return None
        platform = self.platform.value

        def effective():
            # The route Hermes picks for this chat: the most specific enabled one that matches it.
            for r in sorted(routes, key=lambda r: r.specificity, reverse=True):
                if getattr(r, "enabled", True) and r.matches(platform, chat_id=chat_id, user_id=device_id):
                    return r
            return None

        route = effective()
        if route is None:
            route = ProfileRoute(name=f"gadget-virtual-{chat_id}", platform=platform, profile=profile,
                                 chat_id=chat_id)
            routes.append(route)
            routes.sort(key=lambda r: r.specificity, reverse=True)
            self._virtual_owned[chat_id] = route
            logger.info("[%s] routed virtual chat %s to profile %s", self.name, chat_id, profile)
            route = effective()
        if route is None or route.profile != profile:
            # A route of the administrator's sends this chat elsewhere (or nothing matches it): don't
            # take it over. The caller stamps the device's own chat instead, as without virtual mode.
            self._virtual_routes.discard(chat_id)
            return None
        self._virtual_routes.add(chat_id)
        return chat_id

    def _prune_virtual_routes(self, served: set) -> None:
        """Drop the routes this adapter added for profiles no longer served or offered (deleted, parked,
        taken off ``profiles``). Devices on such a profile are sent back to the default one meanwhile."""
        if not served:  # a roster Hermes could not give drops nothing
            return
        offered = served if self._offer_all else served & ((self._offered or set()) | {"default"})
        # The adapter's own routes too, for chats an administrator's route has since taken over.
        tracked = self._virtual_routes | set(self._virtual_owned)
        stale = {c for c in tracked if c.split(VIRTUAL_SEP, 1)[1] not in offered}
        if not stale:
            return
        routes = getattr(getattr(getattr(self, "gateway_runner", None), "config", None), "profile_routes", None)
        owned = [self._virtual_owned.pop(c) for c in stale if c in self._virtual_owned]
        if routes is not None and owned:
            # Only the routes this adapter added: an administrator's route for the same chat stays.
            routes[:] = [r for r in routes if not any(r is o for o in owned)]
        self._virtual_routes -= stale
        logger.info("[%s] dropped virtual routes for profiles no longer offered: %s", self.name, sorted(stale))

    # -- pairing --------------------------------------------------------------------

    def _authorized(self, device_id: str) -> Optional[bool]:
        return self._is_sender_authorized(device_id, "dm", device_id)

    async def is_paired(self, session: DeviceSession) -> bool:
        # None means no runner check is installed (unit harness): nothing to gate on.
        return self._verdict(session) is not False

    def profiles(self, session: Optional[DeviceSession] = None) -> List[Dict[str, str]]:
        """Profiles devices may switch to (the default one first): those ``extra.profiles`` names, or all
        with ``all``, that this multiplexed gateway serves (parked and standalone ones are not).
        Empty when switching is off, and for a ``session`` that a gadget route sends somewhere."""
        if session is not None and self._routed(session):
            return []
        return self._offered_roster() or []

    def _offered_roster(self) -> Optional[List[Dict[str, str]]]:
        """The roster a switching device is offered now; None when Hermes's profiles can't be read (an
        older Hermes, a profile tree it cannot read), which a roster push takes as "unknown", not empty."""
        if not self._switching_on():
            return []
        try:
            from agent.secret_scope import is_multiplex_active
            from hermes_cli.profiles import list_profiles, profiles_to_serve

            if not is_multiplex_active():  # a standalone gateway runs every turn in its own profile
                return []
            served = {name for name, _home in profiles_to_serve(True)}
            infos = list_profiles(lazy_skill_count=True)
        except Exception as exc:  # said once: the pairing watch asks every few seconds
            if not getattr(self, "_roster_unreadable", False):
                logger.warning("[%s] cannot list Hermes profiles: %s", self.name, exc)
            self._roster_unreadable = True
            return None
        self._roster_unreadable = False
        if not self._offer_all:
            served &= self._offered | {"default"}
        roster = [{"id": p.name, "name": _profile_label(p)[:24]} for p in infos if p.name in served]
        return sorted(roster, key=lambda p: p["id"] != "default")

    async def _push_rosters(self) -> None:
        """Send `profiles` to each connected, paired device that takes it and whose offered list changed
        since the welcome or the last push: a profile created, deleted, parked or taken off the offer
        reaches the device within a pass of the pairing watch, without a reconnect."""
        sessions = [s for s in self._hub.sessions.values()
                    if s.caps.get("profiles") is True and s.paired and not s.closed and s.welcomed]
        if not sessions:
            return
        roster = await asyncio.to_thread(self._offered_roster)
        if roster is None:
            return
        offered = {p["id"] for p in roster}
        for session in sessions:
            mine = [] if self._routed(session) else roster
            if mine == session.roster or self._hub.get(session.device_id) is not session or session.closed:
                continue
            try:
                await session.send_profiles(mine, offered or None)
                logger.info("[%s] %s now offered: %s", self.name, session.device_id, [p["id"] for p in mine])
            except Exception as exc:
                logger.debug("[%s] roster push to %s failed: %s", self.name, session.device_id, exc)

    def _busy(self, device_id: str) -> bool:
        """A turn or an unexpired question the adapter knows of. A convenience, not a guarantee:
        a reply already on its way from the old profile may still arrive after a switch."""
        prompts = [p for p in self._prompts.get(device_id) or [] if not p.expired()]
        self._prompts[device_id] = prompts
        return device_id in self._turns or device_id in self._interpret_tasks or bool(prompts)

    def validate_native_profile_route(self, source) -> bool:
        """Revalidate device-selected routing at native admission and durable replay.

        A queued message keeps its selected profile even if the device switches again;
        current owner trust and the offered-profile policy must still permit that turn.
        This does not authorize a device or create an approval in another profile.
        """
        if (not self._switching_on() or source.platform != self.platform
                or source.chat_type != "dm" or not source.user_id
                or source.user_id != source.chat_id or source.profile_route_rejected
                or source.delivered_via_upstream_relay):
            return False
        if source.profile not in {p["id"] for p in self.profiles()}:
            return False
        if not self._owner_trusts(source.user_id):
            return False
        # A non-default selection is owner-bound before select_profile grants it.
        return (source.profile == "default" or
                self._store is not None and self._store.owner_bound(source.user_id) is True)

    def _owner_trusts(self, device_id: str) -> bool:
        """The default profile's verdict on this device; only a literal True counts. Other profiles'
        approvals are their own. The listener's sender check is the default profile's answer only while
        no ``gateway.profile_routes`` entry matches the device: a route makes Hermes read the routed
        profile instead, and Hermes has no public per-profile check yet
        (NousResearch/hermes-agent#143892). A device that switched before can be
        routed later, so a routed device fails closed here rather than let another profile's approval
        stand in for the default one."""
        try:
            routed = self.build_source(chat_id=device_id, chat_type="dm", user_id=device_id)
            if routed.profile is not None or routed.profile_route_rejected is True:
                if device_id not in self._routed_refused:
                    self._routed_refused.add(device_id)
                    logger.warning("[%s] %s answers to the default profile, but gateway.profile_routes sends "
                                   "it elsewhere: refusing it. Remove the route and let it switch profiles "
                                   "instead.", self.name, device_id)
                return False
            check = self._owner_check or self._is_sender_authorized
            return check(device_id, "dm", device_id) is True
        except Exception:
            logger.warning("[%s] authorization check failed for %s", self.name, device_id, exc_info=True)
            return False

    def _refusal(self, session: DeviceSession) -> Optional[str]:
        """Why no pairing code can let this device in, or None: ``routed`` for a device that answers to
        the default profile but a route sends elsewhere (``_owner_trusts`` refuses it)."""
        return "routed" if self._store.owner_bound(session.device_id) and self._routed(session) else None

    def _routed(self, session: DeviceSession) -> bool:
        """A ``gateway.profile_routes`` entry matches this device (Hermes may also reject it)."""
        source = self._routed_source(session)
        return source.profile is not None or source.profile_route_rejected is True

    def _switching_on(self) -> bool:
        """Only the default profile's ``extra.profiles`` turns switching on; another profile's listener
        never offers profiles, so no profile can add itself."""
        default_adapter = (getattr(self, "_owner_profile", None) or "default") == "default"
        return default_adapter and (self._offer_all or self._offered is not None)

    def _switches(self, session: DeviceSession) -> bool:
        """The device picks its profile: switching is on, it can, and no gadget route picks one for it."""
        return self._switching_on() and session.caps.get("profiles") is True and not self._routed(session)

    def _owner_bound(self, session: DeviceSession) -> bool:
        """Devices held to the default profile's verdict: those that switch, and any that switched
        before, whatever they claim in a later hello."""
        return self._switches(session) or self._store.owner_bound(session.device_id)

    def _verdict(self, session: DeviceSession) -> Optional[bool]:
        """Owner-bound devices answer to the default profile alone; others keep the routed check."""
        if self._owner_bound(session):
            return self._owner_trusts(session.device_id)
        return self._authorized(session.device_id)

    def _still_trusted(self, session: DeviceSession) -> bool:
        return (self._hub is not None and self._hub.get(session.device_id) is session
                and session.paired and self._owner_trusts(session.device_id))

    async def select_profile(self, session: DeviceSession, profile: str) -> Optional[str]:
        if self.interpreting(session.device_id) is not None:
            return "busy"  # whoever holds an interpreting gadget does not choose who it answers to
        if self._routed(session):
            return "routed"
        if not self._switches(session):
            return "unknown"  # switching is off, or the device never said it can switch (caps.profiles)
        if profile not in {p["id"] for p in self.profiles()}:
            return "unknown"  # Hermes would quietly run an unknown profile as the default one
        if not self._still_trusted(session):
            return "unpaired"
        if self._busy(session.device_id):
            return "busy"
        if profile != "default":
            # Bound before the first message goes there, so a later hello without caps.profiles still
            # answers to the default profile. Nothing is written into the target profile: it approves the
            # device itself, through its own pairing flow, the first time the device talks to it.
            try:
                await asyncio.to_thread(self._store.bind_to_owner, session.device_id)
            except OSError:
                logger.warning("[%s] cannot bind %s to the default profile", self.name, session.device_id,
                               exc_info=True)
                return "unpaired"
        if not self._still_trusted(session):  # revoked or replaced meanwhile
            return "unpaired"
        session.profile = profile
        logger.info("[%s] %s now talks to profile %s", self.name, session.device_id, profile)
        return None

    async def _reset_profile(self, session: DeviceSession) -> None:
        """Back to the default profile; questions from the old one go (Hermes times them out as denied)."""
        session.profile = "default"  # every state change happens before the first await
        prompts = self._prompts.pop(session.device_id, [])
        self._turns.pop(session.device_id, None)
        self._new_requested.pop(session.device_id, None)
        for prompt in prompts:
            await session.close_prompt(prompt.id)
        await session.send_profile()

    async def face(self, session: DeviceSession) -> Optional[faces.ProfileFace]:
        if not self._profile_faces:
            return None
        if self._convert_avatars is None:
            self._convert_avatars = imaging.pillow_available()
            if not self._convert_avatars:
                logger.info("[%s] Pillow is not installed: devices show profile face files, not avatars", self.name)
        profile = self._face_profile(session)
        look = self._face_from.get(session)
        settled = look.settled if look and look.profile == profile else None
        if settled is None:
            look = self._face_from[session] = _FaceLook(profile)
        if profile is None:
            return None
        try:
            home = self._profile_home(profile)
            # Stat'ed before the read, so a change made while reading shows at the next pairing check.
            now = await asyncio.to_thread(faces.stamp, home, self._convert_avatars)
            if settled is None:
                look.seen = now
            elif now != settled:
                return self._unsettled(look)
            error = None
            try:
                found = await asyncio.to_thread(faces.for_profile, home, self._convert_avatars, session.face_px,
                                                session.face_frames)
            except Exception as exc:  # a file that went while it was read is a change, checked below
                found, error = None, exc
            if settled is not None:
                if await asyncio.to_thread(faces.stamp, home, self._convert_avatars) != settled:
                    return self._unsettled(look)
                look.seen, look.settled = settled, None
            if error:
                raise error
            if found is None or profile == "default":  # the default profile's approval is session.paired
                return found
            # Found while the profile approved the device: a revocation since then makes it stale.
            return replace(found, valid=lambda: self._face_profile(session) == profile)
        except Exception as exc:  # an unreadable picture shows the mascot; never block a connection on it
            logger.warning("[%s] no face for profile %s: %s", self.name, profile, exc)
            return None

    async def thumb(self, session: DeviceSession, profile: str) -> Optional[faces.ProfileFace]:
        """``profile``'s thumbnail for a device's grid: its face file or avatar, as its own face would be.
        Each profile's files are stat'ed on every call and read only when they changed and then held
        still until a call at least THUMB_HOLD_S later, a debounce for a file being written;
        until then the last one stays."""
        if not self._profile_faces or not session.thumb_px:
            return None
        if self._convert_avatars is None:
            self._convert_avatars = imaging.pillow_available()
        key = (profile, session.thumb_px, self._convert_avatars)
        try:
            home = self._profile_home(profile)
            now = await asyncio.to_thread(faces.stamp, home, self._convert_avatars)
            cached = self._thumbs.get(key)
            if cached is not None and cached[0] == now:
                return cached[1]
            if cached is not None:
                # Changed since: read it once it has held still for THUMB_HOLD_S, about a pairing check.
                # Several devices ask in the same check, so the second one to see the change waits too.
                seen = self._thumb_changing.get(key)
                if seen is None or seen[0] != now:
                    self._thumb_changing[key] = (now, time.monotonic())
                    return cached[1]
                if time.monotonic() - seen[1] < THUMB_HOLD_S:
                    return cached[1]
            found = await asyncio.to_thread(faces.thumb_for_profile, home, self._convert_avatars, session.thumb_px)
        except Exception as exc:  # an unreadable profile keeps what the device shows
            logger.debug("[%s] no thumbnail for profile %s: %s", self.name, profile, exc)
            cached = self._thumbs.get(key)
            if cached:
                return cached[1]
            raise  # nothing known yet: not "no thumbnail", which would clear the one the device holds
        self._thumbs[key] = (now, found)
        self._thumb_changing.pop(key, None)
        return found

    @staticmethod
    def _unsettled(look: _FaceLook) -> faces.ProfileFace:
        """The files changed after they settled, before or while they were read: they settle again."""
        look.settled = look.last = None
        return _KEEP_FACE

    @staticmethod
    def _profile_home(profile: str) -> Path:
        if profile == "default":  # this gateway's own home, for a standalone profile too
            from hermes_constants import get_hermes_home
            return get_hermes_home()
        from hermes_cli.profiles import get_profile_dir
        return get_profile_dir(profile)

    async def _follow_face(self, session: DeviceSession) -> Optional[asyncio.Task]:
        """Another profile approving or revoking the device, or its face file or avatar changing, only
        shows at the pairing check: its face follows. Returns the face update it starts, if any."""
        look = self._face_from.get(session)
        if (session.paired and self._profile_faces and look
                and (self._face_profile(session) != look.profile or await self._face_source_changed(session, look))):
            return session.update_face()
        return None

    async def _face_source_changed(self, session: DeviceSession, look: _FaceLook) -> bool:
        """Whether the files the device's last face lookup read from have changed since, and settled:
        the same on two pairing checks in a row, a debounce for a file being written. Stat only; the
        lookup it starts sends a face only if its hash differs from the one the device holds."""
        if look.profile is None or look.seen is None or look.settled is not None:  # settled: not read yet
            return False
        now = await asyncio.to_thread(faces.stamp, self._profile_home(look.profile), self._convert_avatars)
        if self._face_from.get(session) is not look:
            return False  # looked up again meanwhile
        before, look.last = look.last, now
        if now == look.seen or now != before:
            return False
        look.settled = now
        return True

    def _face_profile(self, session: DeviceSession) -> Optional[str]:
        """The profile whose face the device may show: the one answering it. None: the mascot.

        A device's own pick among the offered profiles shows that profile's face once the default profile
        trusts the device, the gate switching itself uses; the profile's own pairing approval isn't needed
        (Hermes has no public per-profile check, NousResearch/hermes-agent#143892). A gadget route's
        profile shows once the listener's own sender check accepts the device: for a routed device that
        check follows the route, so it is that profile's verdict."""
        source = self._source(session)
        if source.profile_route_rejected:
            return None
        if source.profile in (None, "default"):
            return "default"  # session.paired is the default profile's approval
        if self._switches(session):
            return source.profile if self._still_trusted(session) else None
        try:
            accepted = self._is_sender_authorized(session.device_id, "dm", session.device_id) is True
        except Exception:
            logger.warning("[%s] cannot check %s for profile %s", self.name, session.device_id, source.profile,
                           exc_info=True)
            accepted = False
        return source.profile if accepted else None

    def language(self, session: DeviceSession) -> Optional[str]:
        # `platforms.gadget.extra.language`, else the profile's language (HERMES_LANGUAGE > display.language > en).
        if self._language != "auto":
            return self._language
        try:
            return get_language()
        except Exception as exc:  # the gadget falls back to English; never block a connection on this
            logger.debug("Could not read Hermes's language: %s", exc)
            return None

    async def on_ready(self, session: DeviceSession) -> None:
        if session.paired:
            self._store.clear_pairing(session.device_id)
            await self._claim_home(session)
            # The mode first, in so many words: the device keeps its own belief through a lost
            # connection, and one that ended here while it was away must not go on there.
            state = self._interpreting.get(session.device_id)
            if state is not None and state.expired():
                await self.interpret_stop(session.device_id)
                state = None
            await session.send_interpret(state.message() if state is not None else interpret.OFF)
            await self._interpret_release(session)  # a question asked, and messages pushed, while it was away
            await self._push_dashboard(session)
            return
        self._prompts.pop(session.device_id, None)  # unpaired while away: its questions are void
        self._held_pushes.pop(session.device_id, None)  # and so is what waited for its mode to end
        await session.withdraw_choice(f"{session.name} is not paired")
        await self.interpret_stop(session.device_id)
        refusal = self._refusal(session)
        if refusal:  # no pairing code can let it in: say why rather than ask Hermes for one
            await session.set_paired(False, refusal)
            return
        cached = self._store.pairing_for(session.device_id)
        if cached:
            await session.send_pairing(*cached)
            return
        # Hermes answers an unauthorized DM with a pairing code (rate limited per sender);
        # send() recognises it and forwards it to the device as a pairing frame.
        source = self._source(session)
        if self._owner_bound(session):  # pairing happens in the default profile, whatever a route says
            source.profile, source.profile_route_rejected = "default", False
        await self.handle_message(MessageEvent(
            text=PAIRING_TRIGGER, message_type=MessageType.TEXT,
            source=source, message_id=self._message_id(session, "pair")))
        session.spawn(self._pairing_grace(session))

    async def _pairing_grace(self, session: DeviceSession) -> None:
        await asyncio.sleep(PAIRING_GRACE_S)
        if (session.closed or session.paired or session.unpaired_reason
                or self._store.pairing_for(session.device_id)):
            return
        await session.send_notice(
            f"No pairing code yet. On the Hermes host run 'hermes pairing list', or allow "
            f"{session.device_id} in GADGET_ALLOWED_USERS.", ttl_s=60)

    async def _sync_pairing(self, session: DeviceSession, served: set | None = None) -> set | None:
        """One pairing-watch step for one device. Revocation first, with no await before the reset."""
        if session.profile != "default" and not self._owner_trusts(session.device_id):
            await self._reset_profile(session)
        verdict = self._verdict(session)
        if verdict is None:
            return served
        state = self._interpreting.get(session.device_id)
        # Not for a device that is no longer trusted: leaving the mode here would deliver what was
        # held for its owner while it still counts as paired. The revocation below ends its mode.
        if verdict and state is not None and state.expired() and session.device_id not in self._interpret_tasks:
            await self.interpret_stop(session.device_id)  # left in the mode, unused: the device is told
            await session.send_notice("Interpreter off")
        if session.profile != "default":
            if served is None:
                served = {p["id"] for p in await asyncio.to_thread(self.profiles)}
            if session.profile not in served:
                await self._reset_profile(session)
        if verdict and not session.paired:
            self._store.clear_pairing(session.device_id)
            await session.set_paired(True)
            logger.info("[%s] device %s approved", self.name, session.device_id)
            await self._claim_home(session)
            await self._push_dashboard(session)
        elif not verdict:
            refusal = self._refusal(session)
            if session.paired or refusal != session.unpaired_reason:
                self._prompts.pop(session.device_id, None)
                # Leaving the mode delivers what was held for the owner, and the device is still
                # paired at that point: a revoked device gets none of it.
                self._held_pushes.pop(session.device_id, None)
                # A choice the agent asked: an answer from a device that is no longer trusted is none.
                await session.withdraw_choice(f"{session.name} is no longer paired")
                await self.interpret_stop(session.device_id)
                await session.set_paired(False, refusal)
                if refusal is None:  # a pairing code can let it in again
                    await self.on_ready(session)
        await self._follow_face(session)
        if session.thumb_px and session.roster_ids and session.paired:
            if served is None:  # one look at the served profiles per check, shared by every device
                served = {p["id"] for p in await asyncio.to_thread(self.profiles)}
            # A profile's new face file or avatar reaches the grid at the next checks; one parked or taken
            # off the offer since the welcome is left alone. A list Hermes could not give filters nothing.
            session.update_thumbs(served or None)
        else:
            session.update_thumbs()
        return served

    async def _watch_pairing(self) -> None:
        """Mirror pairing approvals and revocations onto connected devices, and changes to the profiles
        they are offered."""
        while True:
            await asyncio.sleep(PAIRING_POLL_S)
            if not self._hub:
                continue
            served = None
            for session in list(self._hub.sessions.values()):
                try:  # one device's closed socket must never stop the watcher for the others
                    served = await self._sync_pairing(session, served)
                except Exception as exc:
                    logger.debug("[%s] pairing sync failed for %s: %s", self.name, session.device_id, exc)
            await self._push_rosters()
            if self._virtual_routes or self._virtual_owned:
                try:
                    if served is None:
                        served = {p["id"] for p in await asyncio.to_thread(self.profiles)}
                    self._prune_virtual_routes(served)
                except Exception as exc:
                    logger.debug("[%s] virtual route cleanup failed: %s", self.name, exc)

    # -- dashboard ----------------------------------------------------------------------

    def _board_changed(self) -> None:
        """A session changed state (called on an agent thread)."""
        loop, wake = self._loop, self._dashboard_wake
        if loop is not None and wake is not None and not loop.is_closed():
            loop.call_soon_threadsafe(wake.set)

    async def _watch_dashboard(self) -> None:
        """Keep dashboards current: at once after a session change, and every few seconds for
        tiles set from the CLI and for what expired. Devices only get what changed."""
        while True:
            try:
                await asyncio.wait_for(self._dashboard_wake.wait(), DASHBOARD_POLL_S)
                await asyncio.sleep(DASHBOARD_SETTLE_S)
            except asyncio.TimeoutError:
                pass
            self._dashboard_wake.clear()
            try:
                await self._push_dashboard()
            except Exception as exc:
                logger.debug("[%s] dashboard update failed: %s", self.name, exc)

    async def _push_dashboard(self, only: Optional[DeviceSession] = None) -> None:
        if self._hub is None or self._tiles is None:
            return
        live = [s for s in self._hub.sessions.values()
                if s.paired and not s.closed and s.dashboard_version]  # each gets the layout it draws
        targets = live if only is None else [s for s in live if s is only]
        if not any(self._switches(s) for s in live):  # no device reads other profiles' boards any more
            self._keep_boards([])
        if not targets:
            return
        switching = [self._switches(s) for s in targets]
        theirs: Optional[List[Dict[str, str]]] = None
        seq = 0
        if any(switching):
            async with self._roster_lock:  # rosters are read, and their rows taken, in push order
                self._push_seq = seq = self._push_seq + 1
                self._pushed_seq.update((s.device_id, seq) for s, sw in zip(targets, switching)
                                        if sw and self._hub is not None and self._hub.get(s.device_id) is s)
                theirs = dashboard.BOARD.sessions(*await self._offered_boards())
        mine = dashboard.BOARD.sessions()  # after any roster wait, so a change made during it goes out
        if theirs is None:
            theirs = mine
        for session, switches in zip(targets, switching):
            if not session.paired or (switches and not self._still_trusted(session)):
                continue  # revoked or replaced while the roster was read
            if switches and self._pushed_seq.get(session.device_id) != seq:
                continue  # a newer push has a newer roster for this device and sends it
            # No await from the check to here: send_dashboard queues on the device's send lock (FIFO)
            # before any newer push can, so an older dashboard never lands after a newer one.
            try:
                msg = dashboard.message_for(session, theirs if switches else mine, self._tiles)
            except OSError as exc:  # the file is busy past its retries: the next look sends it
                logger.debug("[%s] dashboard file unreadable: %s", self.name, exc)
                return
            await session.send_dashboard(msg)

    async def _offered_boards(self) -> List[dashboard.SessionBoard]:
        """The session boards of the other profiles a switching device may pick, as each profile's
        copy of the plugin loads them; a change on one wakes the dashboard like one of ours."""
        offered = [p["id"] for p in await asyncio.to_thread(self.profiles)]
        if self._dashboard_task is None:  # disconnect() began meanwhile and dropped every listener
            return []
        return self._keep_boards([b for b in runtime.boards(offered) if b is not dashboard.BOARD])

    def _keep_boards(self, boards: List[dashboard.SessionBoard]) -> List[dashboard.SessionBoard]:
        """Listen to ``boards`` and our own board only: a profile taken off the offer, or a board
        a reloaded copy of the plugin replaced, no longer wakes the dashboard."""
        for board in [b for b in self._boards if b is not dashboard.BOARD and b not in boards]:
            self._boards.pop(board)()
        for board in boards:
            if board not in self._boards:
                self._boards[board] = board.subscribe(self._board_changed)
        return boards

    # -- firmware updates -------------------------------------------------------------

    async def _watch_updates(self) -> None:
        """Install firmware staged with 'hermes gadget update' once its device is online."""
        queue = ota.UpdateQueue(self._store.path.parent)
        bot_chats = botchat.BotChatQueue(self._store.path.parent)
        while True:
            await asyncio.sleep(UPDATE_POLL_S)
            if not self._hub:
                continue
            for device_id in bot_chats.pending():
                await self._serve_bot_chat_guarded(bot_chats, device_id)
            for device_id in queue.pending():
                session = self._hub.get(device_id)
                running = self._installing.get(device_id)
                if session is None or session.closed or (running and not running.done()):
                    continue
                self._installing[device_id] = session.spawn(self._install_staged(queue, session))

    async def _serve_bot_chat_guarded(self, queue: "botchat.BotChatQueue", device_id: str) -> None:
        """Serve the device's request without ever raising: the update watcher runs this, and must keep
        serving firmware updates and the other requests whatever one request, or the data dir, does."""
        request = None
        try:
            request = queue.load(device_id)
            await self._serve_bot_chat(queue, device_id, request)
        except Exception as exc:
            logger.warning("[%s] bot-chat request for %s failed", self.name, device_id, exc_info=True)
            # Fail only the request that raised: one a newer run wrote meanwhile stays pending. The
            # data dir may be what failed, so recording the failure only logs if it fails too.
            try:
                request_id = (request or {}).get("id")
                if request is not None and queue.drop(device_id, request_id):
                    queue.report(device_id, id=request_id, state="failed", error=str(exc))
            except Exception:
                logger.warning("[%s] could not record the bot-chat failure for %s", self.name, device_id,
                               exc_info=True)

    async def _serve_bot_chat(self, queue: "botchat.BotChatQueue", device_id: str,
                              request: Optional[dict] = None) -> None:
        """Run one 'hermes gadget bot-chat' request (``request``, or the one in ``queue``): switch the
        device to the profile, then send ``/resume --all Bot Chat`` from it, so Hermes moves that
        profile's Bot Chat onto its chat. Hermes decides whether the device may (an admin of this
        platform); the CLI checks the result."""
        if request is None:
            request = queue.load(device_id)
            if request is None:
                queue.drop(device_id)
                return
        request_id = request.get("id")

        def fail(error: str) -> None:
            if queue.drop(device_id, request_id):  # a newer request replaced it: that one reports instead
                queue.report(device_id, id=request_id, state="failed", error=error)

        if time.time() - float(request.get("requested_at") or 0) > botchat.REQUEST_TTL_S:
            fail("the device did not come online in time")
            return
        session = self._hub.get(device_id) if self._hub else None
        # Wait, until the request expires, for a connected, paired device with no turn or question
        # pending: Hermes refuses /resume during a turn, whatever profile the device is on.
        if session is None or session.closed or not session.paired or self._busy(device_id):
            return
        profile = request["profile"]
        if session.profile != profile:
            error = await self.select_profile(session, profile)
            if error == "busy":
                return  # a turn began meanwhile: try again at the next pass
            if error is not None:
                fail(f"cannot switch to {profile}: {error}")
                return
            await session.send_profile()
        # The awaits above let the device, or the pairing watch, change the profile or the connection:
        # the resume must go out on this connection, for the profile asked for, or not at all.
        if self._hub is None or self._hub.get(device_id) is not session or session.closed:
            fail("the device reconnected; run the command again")
            return
        if session.profile != profile:
            fail(f"the device left {profile} (now on {session.profile}); run the command again")
            return
        if self._busy(device_id):
            return
        event = self._event(session, "botchat", botchat.RESUME_COMMAND, message_type=MessageType.TEXT)
        # The profile answers this chat only once it has approved the device itself; until then Hermes
        # takes the resume for a stranger's message and replies with a pairing code. Where the gateway's
        # check can tell (a routed or virtual chat), say so rather than send a resume that can't work.
        approved = None
        if event.source.chat_id != device_id:  # on the device's own chat the check answers for the default profile
            try:
                approved = self._is_sender_authorized(device_id, "dm", event.source.chat_id)
            except Exception:
                approved = None
        if approved is False:
            fail(f"{profile} hasn't approved this gadget yet: talk to {profile} once on the gadget, approve "
                 f"the code it shows (hermes -p {profile} pairing list), then run the command again")
            return
        if not queue.drop(device_id, request_id):
            return  # a newer request replaced this one while it was being served
        try:
            forwarded = await self._forward(session, event)
        except Exception as exc:  # the request is already taken: report it here, the guard can't
            queue.report(device_id, id=request_id, state="failed", error=f"sending the resume failed: {exc}")
            raise
        if not forwarded:
            queue.report(device_id, id=request_id, state="failed",
                         error="the default profile no longer trusts this device")
            return
        queue.report(device_id, id=request_id, state="sent", profile=profile, chat_id=event.source.chat_id)
        logger.info("[%s] sent %r from %s on profile %s", self.name, botchat.RESUME_COMMAND, device_id, profile)

    async def _install_staged(self, queue: ota.UpdateQueue, session: DeviceSession) -> None:
        staged = queue.load(session.device_id)
        if staged is None:
            return
        data, _meta = staged
        reported = 0.0

        def progress(sent: int, total: int) -> None:
            nonlocal reported
            if sent == total or time.monotonic() - reported >= 1.0:
                reported = time.monotonic()
                queue.report(session.device_id, state="sending", sent=sent, size=total)

        logger.info("[%s] installing staged firmware on %s", self.name, session.device_id)
        try:
            version = await session.update_firmware(data, progress=progress)
        except ota.UpdateError as exc:
            # A device that dropped off gets the image again when it's back, a few times.
            retry = exc.code in ota.RETRY_CODES and queue.count_attempt(session.device_id) < ota.MAX_ATTEMPTS
            # Drop before the final report: whoever reads "failed" or "done" finds nothing staged.
            if not retry:
                queue.drop(session.device_id)
            queue.report(session.device_id, state="retrying" if retry else "failed", code=exc.code, error=exc.message)
            logger.warning("[%s] firmware update for %s failed: %s", self.name, session.device_id, exc.message)
            return
        queue.drop(session.device_id)
        queue.report(session.device_id, state="done", version=version)
        logger.info("[%s] %s installed firmware %s", self.name, session.device_id, version)

    # -- live status for other processes ------------------------------------------------

    def _status_devices(self) -> Dict[str, Dict[str, Any]]:
        """Each connected device's live state, as status.json records it."""
        devices: Dict[str, Dict[str, Any]] = {}
        for session in list(self._hub.sessions.values()) if self._hub else []:
            if session.closed:
                continue
            activity = dict(session.activity)
            if activity.get("state") == "ready" and activity.get("ready_after", 0) > time.time():
                activity["state"] = "speaking"  # still playing the audio it buffered
            activity.pop("ready_after", None)
            devices[session.device_id] = {
                "online": True,
                "name": session.name,
                "activity": activity,
                "actions": [a["name"] for a in session.actions][:16],
                "paired": session.paired,
                "profile": session.profile,
                "connected_at": session.connected_at,
                "battery": live_status.battery(session.sensors),
                "sensors_at": session.sensors_at or None,
            }
        return devices

    @staticmethod
    def _status_key(devices: Dict[str, Dict[str, Any]]) -> str:
        """What makes a change worth writing at once: who is online, pairing, profile and the battery's
        percent and power source. Voltage and reading times ride along with the next write."""
        def brief(rec: Dict[str, Any]) -> Dict[str, Any]:
            power = {k: v for k, v in (rec.get("battery") or {}).items() if k != "battery_mv"}
            activity = {k: v for k, v in (rec.get("activity") or {}).items() if k != "at"}
            return {**{k: v for k, v in rec.items() if k not in ("battery", "sensors_at", "activity")},
                    "battery": power, "activity": activity}

        return json.dumps({k: brief(v) for k, v in devices.items()}, sort_keys=True)

    def _write_status(self, devices: Dict[str, Dict[str, Any]], *, running: bool = True) -> bool:
        try:
            live_status.write(self._status_dir, devices, running=running)
        except OSError as exc:
            (logger.debug if self._status_failing else logger.warning)(
                "[%s] could not write %s: %s", self.name, live_status.FILENAME, exc)
            self._status_failing = True
            return False
        self._status_failing = False
        return True

    async def _watch_status(self) -> None:
        """Keep status.json (read by the dashboard's Gadget tab) up to date: rewritten when a device
        connects, leaves, changes pairing or profile, or reports a new battery percent or power source,
        and at least every ``status.REFRESH_S`` so a reader can tell a gateway that stopped."""
        written_key: Optional[str] = None
        written_at = 0.0
        while True:
            try:  # as the other watchers: one bad pass must not end the task, or the tab says "stopped"
                devices = self._status_devices()
                key = self._status_key(devices)
                if key != written_key or time.monotonic() - written_at >= live_status.REFRESH_S:
                    if self._write_status(devices):
                        written_key, written_at = key, time.monotonic()
            except Exception as exc:
                logger.warning("[%s] live status update failed: %s", self.name, exc)
            await asyncio.sleep(STATUS_POLL_S)

    # -- requests from the Hermes Desktop page ----------------------------------------

    async def _watch_desk(self) -> None:
        """Carry cards and device actions from the Desktop page (desk/inbox/) to connected devices."""
        desk = Desk(self._status_dir)
        while True:
            await asyncio.sleep(DESK_POLL_S)
            try:
                batch = await asyncio.to_thread(desk.pending)
            except Exception as exc:
                logger.warning("[%s] Desktop requests unreadable: %s", self.name, exc)
                continue
            for req_id, req in batch:
                try:
                    # An earlier request in this batch can take up to 15 s; one whose page gave up
                    # meanwhile was withdrawn (its file deleted) and must not run late.
                    if not await asyncio.to_thread(desk.waiting, req_id):
                        continue
                    outcome = await self._desk_request(req)
                    await asyncio.to_thread(desk.finish, req_id, outcome)
                except Exception as exc:  # one bad request doesn't hold up the rest
                    logger.warning("[%s] Desktop request failed: %s", self.name, exc)

    async def _desk_request(self, req: Dict[str, Any]) -> Dict[str, Any]:
        if req["kind"] == "interpret" and req.get("off"):  # also for a gadget that is away
            return {"ok": True, "was_on": await self.interpret_stop(req["device"])}
        session = self._hub.get(req["device"]) if self._hub else None
        if session is None or session.closed:
            return {"ok": False, "error": "That gadget isn't connected right now."}
        if req["kind"] == "diag":  # no pairing needed: a device that can't pair is worth a report too
            try:
                got = await session.request_diag()
            except Exception as exc:
                return {"ok": False, "error": str(exc)[:300] or "the device didn't answer"}
            report, when = got["report"], time.time()
            return {"ok": True, "file": gdiag.file_name(report, when),
                    "text": gdiag.report_text(report, got["log"], gdiag.host_log(), when)}
        if not session.paired:
            return {"ok": False, "error": f"{session.name} is waiting for pairing approval."}
        try:
            if req["kind"] == "interpret":
                state = await self.interpret_start(session, req.get("a"), req.get("b"))
                return {"ok": True, "label": state.label, "draws": session.caps.get("interpret") is not None,
                        "speaks": session.has_speaker and self._should_auto_tts_for_chat(session.device_id)}
            if req["kind"] == "card":
                title = textfmt.for_device(str(req.get("title") or ""), session.charset)
                text = textfmt.for_device(str(req.get("text") or ""), session.charset)
                shown = await session.show_card(title, text, int(req.get("seconds") or 0))
                return {"ok": True, "held": shown is False}
            names = {a["name"] for a in session.actions}
            if req["action"] not in names:
                return {"ok": False, "error": f"{session.name} has no action called {req['action']}."}
            result = await session.invoke_action(req["action"], req.get("args") or {}, timeout=15)
            return {"ok": True, "result": result if isinstance(result, (dict, list, str, int, float, bool)) else None}
        except Exception as exc:
            return {"ok": False, "error": str(exc)[:200] or "the device didn't answer"}

    async def _claim_home(self, session: DeviceSession) -> None:
        """Make the first approved device the platform's home channel.

        Without one, Hermes opens every new session with a "type /sethome" notice, which a
        device without a keyboard cannot act on. This does what /sethome would: the running
        config is updated (the runner reads the same object) and config.yaml keeps it.
        """
        if not self._auto_home or self.config.home_channel is not None:
            return
        if str(_get_scoped_secret("GADGET_HOME_CHANNEL", "") or "").strip():
            return
        from gateway import config as gateway_config

        home = gateway_config.HomeChannel(platform=self.platform, chat_id=session.device_id,
                                          name=session.name, user_id=session.device_id)
        self.config.home_channel = home
        try:
            await asyncio.to_thread(gateway_config.persist_home_channel, home)
        except Exception as exc:  # the running gateway still has it; only a restart forgets
            logger.warning("[%s] could not save home channel %s: %s", self.name, session.device_id, exc)
            return
        logger.info("[%s] %s (%s) is now the gadget home channel", self.name, session.name, session.device_id)

    async def _forward_unpaired(self, session: DeviceSession, content: str) -> None:
        if session.unpaired_reason:  # a late reply to a request from before the refusal: no code can let it in
            return
        match = _PAIRING_CMD.search(content or "")
        if match:
            command, code = match.group(1), match.group(2)
            self._store.remember_pairing(session.device_id, code, command, PAIRING_TTL_S)
            await session.send_pairing(code, command)
        else:
            await session.send_notice(textfmt.for_device(content, session.charset), ttl_s=30)

    # -- inbound --------------------------------------------------------------------

    def _routed_source(self, session: DeviceSession):
        return self.build_source(
            chat_id=session.device_id, chat_name=session.name, chat_type="dm",
            user_id=session.device_id, user_name=session.name)

    def _source(self, session: DeviceSession):
        if (self._virtual_profiles and session.profile not in (None, "default")
                and self._switches(session)):
            chat_id = self._virtual_chat(session.device_id, session.profile)
            if chat_id is not None:  # the route stamps source.profile; Hermes admits it as routed
                return self.build_source(
                    chat_id=chat_id, chat_name=f"{session.name} · {session.profile}", chat_type="dm",
                    user_id=session.device_id, user_name=session.name)
        source = self._routed_source(session)
        if self._switches(session):
            source.profile = session.profile  # a multiplexed gateway runs the turn under this profile
        return source

    @staticmethod
    def _message_id(session: DeviceSession, msg_id: str) -> str:
        return f"{session.session_id}:{msg_id or uuid.uuid4().hex[:8]}"

    def _event(self, session: DeviceSession, msg_id: str, text: str, **kw) -> MessageEvent:
        return MessageEvent(
            text=text, source=self._source(session), message_id=self._message_id(session, msg_id),
            user_id=session.device_id, user_name=session.name, channel_prompt=session.prompt_context(), **kw)

    async def _forward(self, session: DeviceSession, event: MessageEvent) -> bool:
        """Hand a device message to Hermes; False when it was dropped here. Work for another profile needs
        the default profile's live trust and the current connection, so neither a revoked device nor a
        replaced connection still finishing an upload reaches another profile; that profile then checks
        its own approval."""
        if (event.source.profile not in (None, "default") and self._owner_bound(session)
                and not self._still_trusted(session)):
            logger.info("[%s] dropped a message from %s for profile %s: not trusted by the default profile",
                        self.name, session.device_id, event.source.profile)
            if self._hub is not None and self._hub.get(session.device_id) is session:
                # The device is waiting for an answer; an old connection's leftovers get none.
                await session.turn_end("", "failure")
                await session.send_notice("Not allowed: this gadget is not approved for that profile")
            return False
        await self.handle_message(event)
        return True

    # -- interpreter mode -------------------------------------------------------------
    #
    # The person holding the gadget may be a stranger, so nothing they say reaches the agent: no
    # MessageEvent is made. The adapter transcribes the recording, asks for the translation through
    # ctx.llm, a plain completion that carries no tools, and speaks it. No session or memory is involved.
    #
    # The mode fails closed. Doubt about whether it is on is resolved toward "not an agent turn":
    # the mode is kept on disk, so a gateway restart does not forget it; the device marks what it
    # sends from the mode, and a marked message is translated or dropped, never answered; while the
    # mode is on, the device shows no Hermes question, takes no answer to one, starts no turn with an
    # event, and messages pushed to it wait.

    def interpreting(self, device_id: str) -> Optional[interpret.Session]:
        """The device's interpreter mode, or None."""
        return self._interpreting.get(device_id)

    def interprets(self, session: DeviceSession) -> bool:
        """For the hub: it shows this device no choice and takes no answer from it meanwhile."""
        return session.device_id in self._interpreting

    def _plugin_llm(self) -> Any:
        return self._llm or runtime.llm(self._registry_key)

    def _interpret_save(self) -> bool:
        """Write which devices interpret to disk; False when that failed, and the file is as it was."""
        if self._interpret_store is None:
            return False
        try:
            self._interpret_store.save(self._interpreting)
        except OSError as exc:
            logger.warning("[%s] could not save which gadgets interpret: %s", self.name, exc)
            return False
        return True

    def _interpret_unavailable(self) -> Optional[str]:
        """Why this Hermes cannot translate, or None: the mode is refused when it is started, not
        found broken at the first sentence."""
        if not callable(getattr(self._plugin_llm(), "complete", None)):
            return ("this Hermes gives plugins no model access (ctx.llm), which interpreter mode translates "
                    "with; update Hermes")
        try:
            from tools.transcription_tools import transcribe_audio
            from tools.tts_tool import text_to_speech_tool  # noqa: F401

            if "source" not in inspect.signature(transcribe_audio).parameters:
                raise ImportError("transcribe_audio takes no source")
        except Exception as exc:
            return f"this Hermes lacks the speech functions interpreter mode uses ({exc}); update Hermes or the plugin"
        return None

    async def interpret_start(self, session: DeviceSession, first: Any, second: Any, *,
                              asked_here: bool = False) -> interpret.Session:
        """Put a paired device in interpreter mode between two languages; the first speaks first.
        ``asked_here``: the agent turn running in the device's own chat asked for it (gadget_interpret),
        and may finish its reply on the device. Any other agent turn running there reaches it no more.
        Raises ValueError with a sentence for a bad language, an unpaired device, a Hermes without
        what the translations need, or a mode that could not be saved."""
        if not session.paired:
            raise ValueError(f"{session.name} is waiting for pairing approval")
        if (missing := self._interpret_unavailable()):
            raise ValueError(missing)
        state = interpret.start(first, second)
        await self._interpret_cancel(session.device_id)  # a translation of the old mode must not touch the new
        before = self._interpreting.get(session.device_id)
        self._interpreting[session.device_id] = state
        if not self._interpret_save():
            # A mode that is not on disk is forgotten by a gateway restart, and the gadget, by then in
            # someone else's hands, would be answered by the agent. So it does not start: the device
            # stays as it was, in the mode it was in before or in none.
            if before is None:
                del self._interpreting[session.device_id]
            else:
                self._interpreting[session.device_id] = before
            raise ValueError("interpreter mode could not be saved, so it was not started: a gateway restart "
                             "would end it unnoticed. Check that the plugin's data folder can be written "
                             "(the gateway log has the error)")
        logger.info("[%s] %s interprets %s", self.name, session.device_id, state.label)
        asker = self._turns.get(session.device_id) if asked_here else None
        if asker is not None:
            self._interpret_asker[session.device_id] = asker
        else:
            self._interpret_asker.pop(session.device_id, None)
        # A choice the agent asked (gadget_ask) cannot wait: its tool call fails, unanswered. First, and
        # with no await since the mode was set: no answer to it is taken in between.
        await session.withdraw_choice(f"{session.name} now interprets, and takes no questions")
        # The owner's audio stops: a gadget_play playback, and speech still being sent.
        session.end_playback("interrupted")
        await session.stop_audio()
        shown = self._prompts.get(session.device_id) or []
        if shown:  # the gadget is about to change hands: its question waits (see _show_prompt)
            await session.close_prompt(shown[0].id)
        await self._interpret_tell(session)
        return state

    async def _interpret_tell(self, session: DeviceSession) -> None:
        """Tell the device the mode it is in now, on or off. Always read here and never carried
        across an await: a start and a stop that overlap must not leave the device, and the mirror
        of its screen (DeviceSession.private), believing something else than this side."""
        state = self._interpreting.get(session.device_id)
        await session.send_interpret(state.message() if state is not None else interpret.OFF)

    async def interpret_stop(self, device_id: str) -> bool:
        """Leave interpreter mode, stopping a translation under way; False when the device was not
        in it. Works for a device that is away: it is told when it connects (on_ready)."""
        was_on = self._interpreting.pop(device_id, None) is not None
        if was_on:
            self._interpret_asker.pop(device_id, None)
        await self._interpret_cancel(device_id)
        session = self._hub.get(device_id) if self._hub else None
        if was_on:
            self._interpret_save()
            logger.info("[%s] %s stopped interpreting", self.name, device_id)
            if session is not None and not session.closed:
                # A mode may have been started again while the translation was ending: then the
                # device is told that one, and nothing is released.
                await self._interpret_tell(session)
                await self._interpret_release(session)
        return was_on

    async def _interpret_release(self, session: DeviceSession) -> None:
        """Out of the mode, the device gets what waited for it: Hermes's open question and the
        messages pushed to it meanwhile."""
        device_id = session.device_id
        if device_id in self._interpreting or not session.paired:
            return
        await self._show_prompt(session)
        # One at a time, oldest first, and a message leaves the queue for good only once it was sent:
        # after a send that failed (the connection dropped), it and the rest wait for the next release.
        # The list itself says whether the queue is still the one this began with: a revocation
        # discards it, and nothing is delivered from, or put back into, a queue that was discarded.
        queue = self._held_pushes.get(device_id)
        while queue and self._held_pushes.get(device_id) is queue:
            if device_id in self._interpreting or not session.paired or session.closed:
                return  # the mode is on again, or the device left: they go on waiting
            held = queue.pop(0)
            chat_id, content, metadata = held
            try:
                sent = (await self.send(chat_id, content, metadata=metadata)).success
            except Exception as exc:
                logger.warning("[%s] could not deliver a held message to %s: %s", self.name, device_id, exc)
                sent = False
            if self._held_pushes.get(device_id) is not queue:
                return  # revoked while it was being sent: what failed is not kept for a later approval
            if not sent:
                queue.insert(0, held)
                return
        if queue is not None and not queue and self._held_pushes.get(device_id) is queue:
            del self._held_pushes[device_id]

    def _owner_output_held(self, device_id: str) -> bool:
        """Whether what Hermes sends the device now is kept from it: while it interprets, all of it
        except the output of the turn that started the mode from the device's own chat."""
        if device_id not in self._interpreting:
            return False
        turn = self._turns.get(device_id)
        return turn is None or turn != self._interpret_asker.get(device_id)

    def _hold_push(self, chat_id: str, content: str, metadata: Optional[Dict[str, Any]]) -> SendResult:
        """Keep a message pushed to an interpreting gadget until the mode ends: whoever holds the
        gadget now may not be its owner."""
        held = self._held_pushes.setdefault(self._device_id(chat_id), [])
        held.append((chat_id, content, metadata))
        if len(held) > MAX_HELD_PUSHES:
            del held[0]
            logger.warning("[%s] %s interprets and holds %d messages; the oldest was dropped",
                           self.name, self._device_id(chat_id), MAX_HELD_PUSHES)
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def _interpret_cancel(self, device_id: str) -> bool:
        """Stop the device's translation under way and its speech; True when there was one."""
        task = self._interpret_tasks.get(device_id)
        if task is None or task is asyncio.current_task():
            return False
        if task.done():
            self._interpret_forget(device_id, task)
            return False
        task.cancel()
        await asyncio.wait({task})  # its end, whatever it was; a cancel of the caller itself still raises here
        self._interpret_forget(device_id, task)
        return True

    def _interpret_forget(self, device_id: str, task: Optional[asyncio.Task]) -> None:
        """A translation that has ended is no longer the device's. Also for one cancelled before it
        ran a single step, whose own cleanup never happened: while an entry stays, the device counts
        as busy, switches no speaker and its mode does not expire."""
        if task is not None and self._interpret_tasks.get(device_id) is task:
            del self._interpret_tasks[device_id]

    async def on_interpret_flip(self, session: DeviceSession) -> None:
        """The device's user switched who speaks next. Not during a translation: it switches already."""
        state = self._interpreting.get(session.device_id)
        if state is None or not session.paired or session.device_id in self._interpret_tasks:
            return
        state.flip()
        self._interpret_save()
        await session.send_interpret(state.message())

    async def on_interpret_stop(self, session: DeviceSession) -> None:
        """CANCEL held on a device in the mode. Whether or not the mode was still on here, the device
        is told it is off and nothing else happens: never a new session."""
        await self.interpret_stop(session.device_id)
        if session.device_id in self._interpreting:
            # Started again while this was being left: the device is in that mode, and its hold of
            # CANCEL ends without an "Interpreter off" that would not be true.
            await self._interpret_tell(session)
            await session.turn_end("", "cancelled")
            return
        await session.refuse_interpreted("Interpreter off")
        await self._interpret_release(session)

    async def on_interpret_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        await self._interpret_content(session, msg_id, text=text, marked=True)

    async def on_interpret_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
        await self._interpret_content(session, msg_id, wav=wav, marked=True)

    async def _interpret_content(self, session: DeviceSession, msg_id: str, *, text: Optional[str] = None,
                                 wav: Optional[bytes] = None, marked: bool) -> bool:
        """Take what a device typed or recorded when it belongs to interpreter mode; False when it is
        an ordinary message for Hermes. ``marked``: the device says it was made in the mode.

        Translated while the mode is on. Dropped, with the device told the mode is off, when the
        device believes in a mode that has ended here (turned off while it was away, or expired), and
        when the mode expires on this very message. Only an unmarked message with the mode off is
        Hermes's to answer."""
        device_id = session.device_id
        state = self._interpreting.get(device_id)
        if state is None and not marked:
            return False
        if not session.paired:  # an unpaired device interprets nothing; its unmarked message pairs as usual
            if marked:
                await session.turn_end("", "cancelled")
            return marked
        if state is not None and state.expired() and device_id not in self._interpret_tasks:
            await self.interpret_stop(device_id)
            await session.refuse_interpreted()
            return True
        if state is not None:
            self._interpret(session, msg_id, state, text=text, wav=wav)
            return True
        if marked:
            await session.refuse_interpreted()
            await self._interpret_release(session)
        return marked

    def _interpret(self, session: DeviceSession, msg_id: str, state: interpret.Session, *,
                   text: Optional[str] = None, wav: Optional[bytes] = None) -> None:
        """Translate one utterance, in place of whichever translation the device was still waiting for."""
        device_id = session.device_id
        previous = self._interpret_tasks.get(device_id)
        if previous is not None and not previous.done():
            previous.cancel()
        task = self._interpret_tasks[device_id] = session.spawn(
            self._interpret_turn(session, msg_id, state, text, wav, previous))
        task.add_done_callback(lambda done: self._interpret_forget(device_id, done))

    async def _interpret_turn(self, session: DeviceSession, msg_id: str, state: interpret.Session,
                              text: Optional[str], wav: Optional[bytes], previous: Optional[asyncio.Task]) -> None:
        device_id, turn, outcome = session.device_id, self._message_id(session, msg_id), "failure"
        try:
            if previous is not None and not previous.done():
                # The turn it replaces ends first, so the device sees one turn at a time. Only that
                # turn's end is waited for: a cancel of this one while it waits still stops it here.
                await asyncio.wait({previous})
            if self._interpreting.get(device_id) is not state:  # left, or started again, while it waited
                outcome = "cancelled"
                return
            # Its own table: an agent turn the owner started before the mode keeps its entry in _turns.
            self._interpret_turns[device_id] = turn
            await session.turn_start(turn)
            state.touch()
            self._interpret_save()
            if wav is not None:
                text = await asyncio.wait_for(self._interpret_transcribe(wav, state.speaker), INTERPRET_STT_DEADLINE_S)
                await session.send_transcript(textfmt.for_device(text, session.charset))
            if not (text or "").strip():
                outcome = "cancelled"
                await session.send_notice("Didn't catch that")
                return
            request = interpret.messages(state, text)
            result = await asyncio.wait_for(asyncio.to_thread(
                self._plugin_llm().complete, request, timeout=INTERPRET_LLM_TIMEOUT_S,
                max_tokens=INTERPRET_MAX_TOKENS, purpose="gadget interpreter"), INTERPRET_LLM_DEADLINE_S)
            translation = str(getattr(result, "text", "") or "").strip()[:interpret.MAX_TRANSLATION_CHARS]
            if self._interpreting.get(device_id) is not state:  # left, or started again, meanwhile
                outcome = "cancelled"
                return
            if not translation:
                raise RuntimeError("the model returned no translation")
            speak = session.has_speaker and self._should_auto_tts_for_chat(device_id)
            await session.send_reply(
                interpret.shown(translation, session.charset, state.target, spoken=speak), turn=turn)
            # The translation is out, so the gadget changes hands now: someone who answers while it
            # is still being spoken is recorded in their own language.
            state.flip()
            self._interpret_save()
            await session.send_interpret(state.message())
            if speak:
                spoken = await self._speak(device_id, translation, wait=True, mode=state)
                if not spoken.success and "interrupted" not in str(spoken.error):
                    await session.send_notice("Couldn't speak the translation")
            outcome = "success"
        except asyncio.CancelledError:
            outcome = "cancelled"
            with contextlib.suppress(Exception):
                await session.stop_audio()
        except Exception as exc:
            reason = "took too long" if isinstance(exc, asyncio.TimeoutError) else str(exc)
            logger.warning("[%s] translation for %s failed: %s", self.name, device_id, reason)
            with contextlib.suppress(Exception):
                await session.send_notice("Translation failed")
        finally:
            if self._interpret_turns.get(device_id) == turn:
                del self._interpret_turns[device_id]
            self._interpret_forget(device_id, asyncio.current_task())
            if not session.closed:
                with contextlib.suppress(Exception):
                    await session.turn_end(turn, outcome)

    async def _interpret_transcribe(self, wav: bytes, language: str) -> str:
        """What the recording says, transcribed in ``language`` by Hermes's speech-to-text.

        The recording may be a stranger's voice and no turn refers to it, so it is not kept: the
        worker that writes the file also deletes it, once the transcription has returned. That
        holds when this task is cancelled or times out meanwhile, since the worker runs on."""
        from tools.transcription_tools import transcribe_audio

        def work() -> dict:
            fd, path = tempfile.mkstemp(prefix="gadget_interpret_", suffix=".wav")
            try:
                with os.fdopen(fd, "wb") as f:
                    f.write(wav)
                runtime.note_clip(path, language)  # the pre_transcription hook hands it to the provider (stt.py)
                return transcribe_audio(path, source="gateway")
            finally:
                with contextlib.suppress(OSError):
                    os.unlink(path)

        result = await asyncio.to_thread(work)
        if not result.get("success"):
            raise RuntimeError(result.get("error") or "speech-to-text failed")
        return str(result.get("transcript") or "").strip()

    async def on_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        if await self._interpret_content(session, msg_id, text=text, marked=False):
            return  # firmware that does not mark what it sends, on a device this side knows interprets
        await self._forward(session, self._event(session, msg_id, text, message_type=MessageType.TEXT))

    async def on_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
        """Cache a finished recording and hand it to Hermes as a voice message, noting its STT language."""
        if await self._interpret_content(session, msg_id, wav=wav, marked=False):
            return
        path = await cache_audio_from_bytes_async(wav, ".wav")
        if self._stt_language:
            runtime.note_clip(path, self._stt_language)
        logger.debug("[%s] %.1fs utterance from %s -> %s", self.name, seconds, session.device_id, path)
        await self._forward(session, self._event(
            session, msg_id, "", message_type=MessageType.VOICE, media_urls=[path], media_types=["audio/wav"]))

    async def on_cancel(self, session: DeviceSession) -> None:
        if await self._interpret_cancel(session.device_id):
            return  # the turn was a translation: it is stopped here, and Hermes has no turn to stop
        if session.device_id not in self._turns:
            # No turn is running (the reply may still be playing after turn.end): the hub
            # already stopped the audio, and a "/stop" now would start a turn of its own.
            return
        await self._forward(session, self._event(session, "cancel", "/stop", message_type=MessageType.TEXT))

    async def on_new_session(self, session: DeviceSession) -> None:
        if session.device_id in self._interpreting:
            # Firmware that does not know `interpret.stop`: its hold of CANCEL leaves the mode too,
            # and is never a new conversation.
            await self.on_interpret_stop(session)
            return
        self._new_requested[session.device_id] = time.monotonic()
        await self._forward(session, self._event(session, "new", "/new", message_type=MessageType.TEXT))

    async def on_event(self, session: DeviceSession, name: str, data: Any, notify: bool) -> None:
        logger.info("[%s] event from %s: %s %s", self.name, session.device_id, name, data)
        if not notify:
            return
        if session.device_id in self._interpreting:
            return  # an event starts an agent turn; none starts from a gadget that interprets
        payload = json.dumps(data, separators=(",", ":"))[:500] if data is not None else ""
        await self._forward(session, self._event(
            session, f"ev-{uuid.uuid4().hex[:6]}", f"[Gadget event] {name} {payload}".strip(),
            message_type=MessageType.TEXT, allow_gateway_control=False))

    async def on_state(self, session: DeviceSession, sensors: dict) -> None:
        # Only paired devices: an unpaired one could otherwise write files at will.
        if not self._battery_log or not session.paired:
            return
        try:
            self._battery_log.record(session.device_id, sensors)
        except OSError as exc:
            logger.warning("[%s] could not write the battery log for %s: %s", self.name, session.device_id, exc)

    async def on_disconnect(self, session: DeviceSession) -> None:
        # A hub already cleared by disconnect() holds no session: the device left all the same.
        if self._hub is None or session.device_id not in self._hub.sessions:
            await self._interpret_cancel(session.device_id)  # nobody is left to hear it
            self._turns.pop(session.device_id, None)
            self._pushed_seq.pop(session.device_id, None)
            if self._store is not None:
                try:  # last seen is when it left, not when it came (hermes gadget devices, the dashboard tab)
                    self._store.touch(session.device_id)
                except OSError as exc:
                    logger.warning("[%s] could not record when %s left: %s", self.name, session.device_id, exc)

    # -- turn lifecycle -------------------------------------------------------------

    async def on_processing_start(self, event: MessageEvent) -> None:
        session = self._session(event.source.chat_id)
        if session is None or not session.paired:
            return
        turn = event.message_id or uuid.uuid4().hex[:12]
        self._turns[session.device_id] = turn
        await session.turn_start(turn)

    async def on_processing_complete(self, event: MessageEvent, outcome) -> None:
        device_id = self._device_id(event.source.chat_id)
        turn = self._turns.pop(device_id, None) or event.message_id or ""
        if self._interpret_asker.get(device_id) == turn:
            del self._interpret_asker[device_id]  # its reply is out: the next turn is let through no more
        session = self._session(event.source.chat_id)
        if session is None or not session.paired:
            return
        await session.turn_end(turn, getattr(outcome, "value", str(outcome)))

    def set_status_text(self, chat_id: str, text: Optional[str]) -> None:
        super().set_status_text(chat_id, text)
        session = self._session(chat_id)
        if session is None or self._loop is None or self._owner_output_held(session.device_id):
            return

        async def send_status() -> None:
            # Looked at again where it is sent: the mode can start between Hermes's thread and this loop.
            if not self._owner_output_held(session.device_id):
                await session.send_status(text or "")

        coro = send_status()
        try:
            running = asyncio.get_running_loop()
        except RuntimeError:
            running = None
        if running is self._loop:
            session.spawn(coro)
        else:
            asyncio.run_coroutine_threadsafe(coro, self._loop)

    # -- outbound text ----------------------------------------------------------------

    async def send(self, chat_id: str, content: str, reply_to: Optional[str] = None,
                   metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        session = self._session(chat_id)
        if session is None:
            return SendResult(success=False, error=f"gadget {chat_id} is not connected")
        if not session.paired:
            await self._forward_unpaired(session, content)
            return SendResult(success=True, message_id=uuid.uuid4().hex[:12])
        if session.profile != "default" and (match := _PAIRING_CMD.search(content or "")):
            # The profile it switched to doesn't know this device yet: Hermes offered that profile's own
            # pairing code. Show the command to approve it there; no turn runs, so settle the device.
            await session.turn_end("", outcome="cancelled")
            await session.send_reply(textfmt.for_device(
                f"Not approved for the {session.profile} profile yet. On the Hermes host run:\n{match.group(1)}",
                session.charset))
            return SendResult(success=True, message_id=uuid.uuid4().hex[:12])
        if self._owner_output_held(session.device_id) or (
                session.device_id in self._interpreting and (metadata or {}).get("job_id")):
            # Outside the turn that started the mode here (another turn, send_message, cron): it waits
            # until the mode ends.
            return self._hold_push(chat_id, content, metadata)
        heard = transcript_echo(content or "")
        if heard is not None:
            # Show what speech-to-text heard as the user's line, not as a reply.
            await session.send_transcript(textfmt.for_device(heard, session.charset))
            return SendResult(success=True, message_id=uuid.uuid4().hex[:12])
        text = textfmt.for_device(content, session.charset)
        if text:
            interim = bool((metadata or {}).get("_interim_send"))
            await session.send_reply(text, turn=self._turns.get(session.device_id), interim=interim)
        # Hermes speaks only replies to voice turns, so a message pushed outside a turn (send_message,
        # cron delivery) would stay silent on a device that has no screen worth reading. Speak it.
        # During a turn the text is the turn's own reply, which Hermes speaks itself when the turn was
        # spoken. Cron output is never part of a turn, so it is queued to follow the turn.
        meta = metadata or {}
        if ((self._turns.get(session.device_id) is None or meta.get("job_id")) and not meta.get("_interim_send")
                and not meta.get(_NOT_SPOKEN) and session.has_speaker and self._should_auto_tts_for_chat(chat_id)
                and (spoken := spoken_message(content, meta))):
            session.spawn(self.speak_queued(chat_id, spoken))
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def speak_queued(self, chat_id: str, text: str) -> SendResult:
        """Speak ``text`` after anything the device is already saying or doing, one message at a time.

        Waits for a turn in progress and for audio already playing, so a message never cuts off a
        spoken reply or an earlier message. A message that has waited SPEECH_QUEUE_WAIT_S since it
        was queued, behind other messages or a busy device, stays text only."""
        device_id = self._device_id(chat_id)
        lock = self._speech_locks.setdefault(device_id, asyncio.Lock())
        deadline = time.monotonic() + SPEECH_QUEUE_WAIT_S
        try:
            await asyncio.wait_for(lock.acquire(), max(0.0, deadline - time.monotonic()))
        except asyncio.TimeoutError:
            return self._not_spoken(device_id)
        try:
            if not await self._device_idle(chat_id, deadline):
                return self._not_spoken(device_id)
            if not self._should_auto_tts_for_chat(chat_id):  # voice was turned off while it waited
                return SendResult(success=False, error="voice is off")
            return await self._speak(chat_id, text, wait=True, idle_by=deadline)
        finally:
            lock.release()

    async def _device_idle(self, chat_id: str, deadline: float) -> bool:
        """Wait until the device is in no turn and plays nothing; False once ``deadline`` passes.
        Returns without yielding when it is idle already, so the caller can start audio at once."""
        device_id = self._device_id(chat_id)
        while True:
            session = self._session(chat_id)
            if session is None or (device_id not in self._turns and not session.audio_playing):
                return True
            if time.monotonic() >= deadline:
                return False
            await asyncio.sleep(0.2)

    def _not_spoken(self, device_id: str) -> SendResult:
        logger.warning("[%s] %s stayed busy for %d s; a message was shown but not spoken",
                       self.name, device_id, int(SPEECH_QUEUE_WAIT_S))
        return SendResult(success=False, error="the device stayed busy")

    def supports_draft_streaming(self, chat_type=None, metadata=None, chat_id=None) -> bool:
        return True

    async def send_draft(self, chat_id: str, draft_id: int, content: str,
                         metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        if self._owner_output_held(session.device_id):
            return SendResult(success=False, error="the gadget is in interpreter mode")
        await session.send_delta(textfmt.for_device(content, session.charset), turn=self._turns.get(session.device_id))
        return SendResult(success=True)

    async def edit_message(self, chat_id: str, message_id: str, content: str, *, finalize: bool = False) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        text = textfmt.for_device(content, session.charset)
        if str(message_id).startswith(PROMPT_PREFIX):
            # Hermes edits a question card when it times out: withdraw it and say why.
            await self._withdraw_prompt(session, str(message_id)[len(PROMPT_PREFIX):])
            if text and session.device_id not in self._interpreting:  # the question was never shown there
                await session.send_notice(text, ttl_s=15)
            return SendResult(success=True, message_id=message_id)
        if self._owner_output_held(session.device_id):
            # A turn's streamed reply: the finished text waits like a pushed message, a draft is dropped.
            return self._hold_push(chat_id, content, None) if finalize else SendResult(
                success=False, error="the gadget is in interpreter mode")
        turn = self._turns.get(session.device_id)
        if finalize:
            await session.send_reply(text, turn=turn)
        else:
            await session.send_delta(text, turn=turn)
        return SendResult(success=True, message_id=message_id)

    # -- questions: slash-command confirmations and command approvals -----------------

    async def send_slash_confirm(self, chat_id: str, title: str, message: str, session_key: str,
                                 confirm_id: str, metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        from tools import slash_confirm

        pending = slash_confirm.get_pending(session_key) or {}
        requested = self._new_requested.pop(session.device_id, None)
        if (pending.get("command") == "new" and pending.get("confirm_id") == confirm_id
                and requested is not None and time.monotonic() - requested <= AUTO_CONFIRM_NEW_S):
            # The device asked for this /new itself (CANCEL held for two seconds).
            session.spawn(self._answer_slash(session, session_key, confirm_id, "once"))
            return SendResult(success=True, message_id=uuid.uuid4().hex[:12])
        head, detail = confirm_text(title, message, session.charset)
        prompt = _Prompt(id=uuid.uuid4().hex[:8], kind="slash", session_key=session_key, title=head,
                         text=detail, confirm_id=confirm_id, ttl_s=SLASH_CONFIRM_TTL_S)
        return await self._ask(session, prompt)

    async def _send_exec_approval_prompt(self, prompt: ExecApprovalPrompt) -> SendResult:
        session = self._session(prompt.chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        command = shown_command(prompt.command, session.display)
        if command is None:
            question = _Prompt(id=uuid.uuid4().hex[:8], kind="approval", session_key=prompt.session_key,
                               title="Can't show this command", text=HOST_ONLY_TEXT, host_only=True,
                               ttl_s=compat.approval_timeout_s())
        else:
            text = command
            if prompt.description:
                text = f"{text}\n{textfmt.for_device(prompt.description, session.charset)}"
            question = _Prompt(id=uuid.uuid4().hex[:8], kind="approval", session_key=prompt.session_key,
                               title="Allow this command?", text=text, ttl_s=compat.approval_timeout_s())
        return await self._ask(session, question)

    async def _ask(self, session: DeviceSession, prompt: _Prompt) -> SendResult:
        queue = self._prompts.setdefault(session.device_id, [])
        if prompt.kind == "slash":
            # Hermes keeps one confirmation per session: a new one supersedes the last.
            queue[:] = [p for p in queue if not (p.kind == "slash" and p.session_key == prompt.session_key)]
        queue.append(prompt)
        await self._show_prompt(session)
        return SendResult(success=True, message_id=PROMPT_PREFIX + prompt.id)

    async def _show_prompt(self, session: DeviceSession) -> None:
        """Put the oldest open question on the device (it shows one at a time)."""
        queue = self._prompts.get(session.device_id) or []
        queue[:] = [p for p in queue if not p.expired()]
        if session.device_id in self._interpreting:
            return  # not on a gadget that may be in someone else's hands: it waits, or is answered elsewhere
        if queue:
            head = queue[0]
            await session.ask(head.id, head.title, head.text, ttl_s=head.ttl_s)

    def _take_prompt(self, device_id: str, prompt_id: str) -> Optional[_Prompt]:
        queue = self._prompts.get(device_id) or []
        for i, prompt in enumerate(queue):
            if prompt.id == prompt_id:
                return queue.pop(i)
        return None

    async def _withdraw_prompt(self, session: DeviceSession, prompt_id: str) -> None:
        queue = self._prompts.get(session.device_id) or []
        was_shown = bool(queue) and queue[0].id == prompt_id
        if self._take_prompt(session.device_id, prompt_id) and was_shown:
            await session.close_prompt(prompt_id)
            await self._show_prompt(session)

    async def on_prompt_reply(self, session: DeviceSession, prompt_id: str, yes: bool) -> None:
        # Unpaired since the question was shown, or no longer trusted by the default profile: it answers nothing.
        if not session.paired or (self._owner_bound(session) and not self._still_trusted(session)):
            return
        if session.device_id in self._interpreting:
            return  # no answer to Hermes is taken from a gadget in interpreter mode
        prompt = self._take_prompt(session.device_id, prompt_id)
        if prompt is not None and prompt.host_only and yes:
            self._prompts.setdefault(session.device_id, []).insert(0, prompt)  # still open on the host
        elif prompt is None or prompt.expired():
            await session.send_notice("That question has expired")
        elif prompt.kind == "slash":
            await self._answer_slash(session, prompt.session_key, prompt.confirm_id, "once" if yes else "cancel")
        else:
            from tools.approval import resolve_gateway_approval

            if resolve_gateway_approval(prompt.session_key, "once" if yes else "deny"):
                await session.send_notice("Approved" if yes else "Denied")
            else:
                await session.send_notice("That approval has expired")
        await self._show_prompt(session)

    async def _answer_slash(self, session: DeviceSession, session_key: str, confirm_id: str, choice: str) -> None:
        from tools import slash_confirm

        try:
            result = await slash_confirm.resolve(session_key, confirm_id, choice)
        except Exception as exc:  # the handler already logs; keep the device informed
            logger.warning("[%s] confirmation failed for %s: %s", self.name, session.device_id, exc)
            result = None
        if result:
            await self.send(session.device_id, result)
        else:
            await session.send_notice("That question has expired")

    async def send_clarify(self, chat_id: str, question: str, choices: Optional[list], clarify_id: str,
                           session_key: str, metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        """A question from the agent (Hermes's clarify tool). The device shows only yes/no prompts,
        so the question goes out as Hermes's own text (numbered choices; the next message, spoken
        or typed, is the answer) and, on a device that speaks replies, is also spoken: otherwise a
        voice user hears nothing while the agent waits."""
        result = await super().send_clarify(chat_id=chat_id, question=question, choices=choices,
                                            clarify_id=clarify_id, session_key=session_key,
                                            metadata={**(metadata or {}), _NOT_SPOKEN: True})
        session = self._session(chat_id)
        if result.success and session is not None and session.paired and self._should_auto_tts_for_chat(chat_id):
            session.spawn(self._speak(chat_id, spoken_question(question, choices)))
        return result

    async def _speak(self, chat_id: str, text: str, *, wait: bool = False, idle_by: Optional[float] = None,
                     mode: Optional[interpret.Session] = None) -> SendResult:
        """Speak ``text`` on the device with Hermes's configured text-to-speech.

        Long text comes back as several files; each one waits for the one before it to finish, so
        they never cut each other off. ``wait`` also waits for the last one. ``idle_by`` is for
        queued speech: each file starts only once the device is idle, or not at all after that time.
        ``mode`` is the interpreter mode the text is a translation of, None for Hermes's own speech;
        nothing is played once the device's mode is another (see _play_file)."""
        if (refused := self._wrong_mode(chat_id, mode)):
            return refused  # before the synthesis, which would be for nobody
        paths: List[str] = []
        try:
            from tools.tts_tool import text_to_speech_tool

            job = asyncio.get_running_loop().run_in_executor(None, lambda: text_to_speech_tool(text=text))
            try:
                raw = await asyncio.shield(job)
            except asyncio.CancelledError:
                job.add_done_callback(_discard_speech)  # the file it is still writing is nobody's
                raise
            result = json.loads(raw) if raw else {}
            paths = [str(p) for p in (result.get("file_paths") or [result.get("file_path")]) if p]
            if not result.get("success") or not paths:
                error = result.get("error") or "text-to-speech failed"
                logger.warning("[%s] could not speak: %s", self.name, error)
                return SendResult(success=False, error=error)
            played = SendResult(success=False, error="nothing to play")
            for i, path in enumerate(paths):
                played = await self._play_file(chat_id, path, wait=wait or i < len(paths) - 1, idle_by=idle_by,
                                               mode=mode)
                if not played.success:
                    break
            return played
        except Exception as exc:  # TTS unavailable or misconfigured: the text still went out
            logger.warning("[%s] could not speak: %s", self.name, exc)
            return SendResult(success=False, error=str(exc))
        finally:
            for path in paths:
                with contextlib.suppress(OSError):
                    os.unlink(path)

    async def get_chat_info(self, chat_id: str) -> Dict[str, Any]:
        session = self._session(chat_id)
        return {"name": session.name if session else chat_id, "type": "dm", "chat_id": chat_id}

    # -- outbound audio ---------------------------------------------------------------

    def _should_auto_tts_for_chat(self, chat_id: str) -> bool:
        # Speaking devices answer voice with voice by default; an explicit
        # "/voice off" in this chat still wins.
        if chat_id in self._auto_tts_disabled_chats:
            return False
        session = self._session(chat_id)
        if session is not None and session.has_speaker and self._speak_replies:
            return True
        return super()._should_auto_tts_for_chat(chat_id)

    async def _play_file(self, chat_id: str, path: str, *, wait: bool = False, idle_by: Optional[float] = None,
                         mode: Optional[interpret.Session] = None) -> SendResult:
        """Play a sound file on the device. ``mode`` says whose audio it is: None for Hermes's own
        (a spoken message, a question, a voice file), or the interpreter mode it is a translation
        of. It plays only while that is the device's mode: speech queued or still being synthesized
        when the gadget changes hands is not played to whoever holds it, and a translation is not
        played after its mode has ended."""
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        if not session.has_speaker:
            return SendResult(success=False, error="gadget has no speaker")
        try:
            pcm = await asyncio.to_thread(gaudio.decode_file, path, session.speaker_rate)
        except Exception as exc:  # undecodable file, missing ffmpeg, I/O error
            logger.warning("[%s] cannot play %s: %s", self.name, path, exc)
            return SendResult(success=False, error=str(exc))
        if idle_by is not None:
            # Synthesis and decoding took time: a turn or its reply may have started meanwhile.
            if not await self._device_idle(chat_id, idle_by):
                return self._not_spoken(session.device_id)
            if not self._should_auto_tts_for_chat(chat_id):
                return SendResult(success=False, error="voice is off")
        # Last, with no await before the audio starts: decoding and the wait above took time.
        if (refused := self._wrong_mode(chat_id, mode)):
            return refused
        out = await session.play_pcm(pcm, session.speaker_rate, turn=self._interpret_turns.get(session.device_id)
                                     or self._turns.get(session.device_id))
        if wait:
            await out.wait_played()
            if out.aborted:
                return SendResult(success=False, error="interrupted by newer audio")
            if out.failed:
                return SendResult(success=False, error="the connection dropped while playing")
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    def _wrong_mode(self, chat_id: str, mode: Optional[interpret.Session]) -> Optional[SendResult]:
        """Why audio made for ``mode`` (None: Hermes's own) is not played now, or None when it is."""
        if self._interpreting.get(self._device_id(chat_id)) is mode:
            return None
        return self._interpret_refusal(chat_id) or SendResult(success=False, error="interpreter mode has ended")

    def _interpret_refusal(self, chat_id: str) -> Optional[SendResult]:
        """Why audio or a picture is not sent now: the gadget interprets, and may not be with its owner."""
        if self._device_id(chat_id) in self._interpreting:
            return SendResult(success=False, error="the gadget is in interpreter mode; try again when it has left it")
        return None

    async def play_tts(self, chat_id: str, audio_path: str, **kwargs) -> SendResult:
        return self._interpret_refusal(chat_id) or await self._play_file(chat_id, audio_path)

    async def send_voice(self, chat_id: str, audio_path: str, caption: Optional[str] = None,
                         reply_to: Optional[str] = None, metadata: Optional[Dict[str, Any]] = None,
                         **kwargs) -> SendResult:
        if (refused := self._interpret_refusal(chat_id)):
            return refused
        result = await self._play_file(chat_id, audio_path)
        if caption:  # shown, not spoken: speech would cut off the file that just started
            await self.send(chat_id, caption, metadata={**(metadata or {}), _NOT_SPOKEN: True})
        return result

    def supports_streaming_tts(self, chat_id: str, audio_format: AudioFormat) -> bool:
        session = self._session(chat_id)
        return bool(session and session.paired and session.has_speaker
                    and not self._owner_output_held(session.device_id)
                    and audio_format.sample_width == 2 and audio_format.channels in (1, 2))

    async def begin_streaming_tts(self, chat_id: str, audio_format: AudioFormat,
                                  metadata: Optional[Dict[str, Any]] = None) -> Optional[StreamingTTSHandle]:
        session = self._session(chat_id)
        if session is None or not self.supports_streaming_tts(chat_id, audio_format):
            return None
        out = await session.open_audio(audio_format.sample_rate, audio_format.channels,
                                       turn=self._turns.get(session.device_id))
        return GadgetTTSHandle(chat_id=chat_id, audio_format=audio_format, out=out)

    async def write_streaming_tts(self, handle: StreamingTTSHandle, chunk: bytes) -> None:
        if not handle.aborted and isinstance(handle, GadgetTTSHandle):
            handle.out.write(chunk)

    async def finish_streaming_tts(self, handle: StreamingTTSHandle, *, interrupted: bool = False) -> None:
        if not isinstance(handle, GadgetTTSHandle):
            return
        if interrupted:
            await handle.out.abort()
        else:
            handle.out.finish()

    async def abort_streaming_tts(self, handle: StreamingTTSHandle, error: Optional[str] = None) -> None:
        if isinstance(handle, GadgetTTSHandle) and not handle.aborted:
            handle.aborted = True
            await handle.out.abort()

    # -- outbound images ---------------------------------------------------------------

    async def send_image_file(self, chat_id: str, image_path: str, caption: Optional[str] = None,
                              reply_to: Optional[str] = None, metadata: Optional[Dict[str, Any]] = None,
                              **kwargs) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
        if (refused := self._interpret_refusal(chat_id)):
            return refused
        box = session.image_box
        if box is None or not imaging.pillow_available():
            await session.send_notice("This gadget cannot display images")
        else:
            img = await asyncio.to_thread(imaging.to_rgb565, image_path, box[0], box[1])
            # The conversion took time: the device may have left, been unpaired or changed hands.
            if session.closed or not session.paired:
                return SendResult(success=False, error="gadget not connected")
            if (refused := self._interpret_refusal(chat_id)):
                return refused
            await session.show_image(img.width, img.height, img.rgb565)
        if caption:
            await self.send(chat_id, caption, metadata=metadata)
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def send_image(self, chat_id: str, image_url: str, caption: Optional[str] = None,
                         reply_to: Optional[str] = None, metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        try:
            path = await cache_image_from_url(image_url)
        except Exception as exc:
            logger.info("[%s] image download failed (%s); sending caption only", self.name, exc)
            return await self.send(chat_id, caption or "(image unavailable)", metadata=metadata)
        return await self.send_image_file(chat_id, path, caption=caption, metadata=metadata)
