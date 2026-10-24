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

Secrets (``~/.hermes/.env``): ``GADGET_ACCESS_TOKEN`` (optional shared token).
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os
import re
import ssl
import time
import uuid
from dataclasses import dataclass, field, replace
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
from . import compat, dashboard, imaging, ota, runtime, textfmt
from . import face as faces
from .battery_log import BatteryLog
from .hub import DeviceHub, DeviceSession, HubDelegate
from .store import DeviceStore

logger = logging.getLogger(__name__)

PLATFORM_NAME = "gadget"
MAX_MESSAGE_LENGTH = 4000
PAIRING_TTL_S = 3600          # mirrors Hermes's pairing-code lifetime
PAIRING_POLL_S = 2.0
PAIRING_GRACE_S = 6.0
UPDATE_POLL_S = 3.0           # how often staged firmware ('hermes gadget update') is looked for
DASHBOARD_POLL_S = 2.0        # how often tiles ('hermes gadget tile') and expiries are looked at
DASHBOARD_SETTLE_S = 0.3      # session changes this close together go out as one dashboard
# Trigger text for Hermes's unauthorized-DM flow. Never reaches the agent while the
# device is unpaired; if approval races it, it is a cheap read-only command.
PAIRING_TRIGGER = "/status"
HOST_ONLY_TEXT = "Can't show all of it here. Approve or deny it on the host."
_PAIRING_CMD = re.compile(r"(hermes\s+(?:-p\s+\S+\s+)?pairing\s+approve\s+\S+\s+([A-Z2-9]{8}))\b")
# The gateway echoes speech-to-text results with this translated line: a microphone emoji and the
# quoted transcript, quoted per language (Ukrainian uses «»). Patterns are built from the line itself.
_ECHO_KEY = "gateway.voice.transcript_echo_short"
_ECHO_SLOT = "\x00"
_echo_patterns: Dict[str, Optional[re.Pattern]] = {}
# Holding CANCEL already is a deliberate "start over", so the /new it sends confirms itself.
AUTO_CONFIRM_NEW_S = 15.0
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


def spoken_question(question: str, choices: Optional[list]) -> str:
    """A clarify question as it is read aloud: the question, then its choices by number."""
    text = (question or "").strip()
    picks = [str(c).strip() for c in choices or [] if str(c).strip()]
    if picks:
        listed = " ".join(f"{i}: {c.rstrip('.')}." for i, c in enumerate(picks, start=1))
        text = f"{text} The choices are {listed} Say the number or your own answer."
    return text


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


class GadgetAdapter(BasePlatformAdapter, HubDelegate):
    MAX_MESSAGE_LENGTH = MAX_MESSAGE_LENGTH
    supports_status_text = True

    def __init__(self, config: PlatformConfig):
        super().__init__(config=config, platform=Platform(PLATFORM_NAME))
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
        # The language `welcome` names for devices left on auto: Hermes's own unless set here.
        self._language = str(extra.get("language") or "auto").strip().lower() or "auto"
        self._tiles: Optional[dashboard.TileStore] = None
        self._dashboard_task: Optional[asyncio.Task] = None
        self._dashboard_wake: Optional[asyncio.Event] = None
        self._boards: Dict[dashboard.SessionBoard, Callable[[], None]] = {}  # each board read, its unsubscribe
        self._roster_lock = asyncio.Lock()  # one roster read at a time; never held across a send
        self._push_seq = 0
        self._pushed_seq: Dict[str, int] = {}  # device -> the newest push that read a roster for it
        self._face_from: Dict[str, Optional[str]] = {}  # device -> the profile its last face lookup could show
        self._battery_log: Optional[BatteryLog] = None
        self._tls_cert = extra.get("tls_cert")
        self._tls_key = extra.get("tls_key")
        self._access_token = extra_or_secret(extra, "access_token", "GADGET_ACCESS_TOKEN") or None
        self._hub: Optional[DeviceHub] = None
        self._store: Optional[DeviceStore] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._turns: Dict[str, str] = {}
        self._watch_task: Optional[asyncio.Task] = None
        self._update_task: Optional[asyncio.Task] = None
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
        for task in (self._watch_task, self._update_task, self._dashboard_task):
            if task:
                task.cancel()
        self._watch_task = self._update_task = self._dashboard_task = None
        if self._hub:
            await self._hub.stop()
            self._hub = None
        self._pushed_seq.clear()  # after the stop: a push still finishing while it ran may add one
        self._mark_disconnected()

    @property
    def hub(self) -> Optional[DeviceHub]:
        return self._hub

    @property
    def loop(self) -> Optional[asyncio.AbstractEventLoop]:
        return self._loop

    def _session(self, chat_id: str) -> Optional[DeviceSession]:
        return self._hub.get(str(chat_id)) if self._hub else None

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
        if not self._switching_on() or (session is not None and self._routed(session)):
            return []
        try:
            from agent.secret_scope import is_multiplex_active
            from hermes_cli.profiles import list_profiles, profiles_to_serve

            if not is_multiplex_active():  # a standalone gateway runs every turn in its own profile
                return []
            served = {name for name, _home in profiles_to_serve(True)}
            infos = list_profiles(lazy_skill_count=True)
        except Exception as exc:  # an older Hermes, or a profile tree it cannot read: no switching
            logger.warning("[%s] cannot list Hermes profiles: %s", self.name, exc)
            return []
        if not self._offer_all:
            served &= self._offered | {"default"}
        roster = [{"id": p.name, "name": (p.display_name or p.name)[:24]} for p in infos if p.name in served]
        return sorted(roster, key=lambda p: p["id"] != "default")

    def _busy(self, device_id: str) -> bool:
        """A turn or an unexpired question the adapter knows of. A convenience, not a guarantee:
        a reply already on its way from the old profile may still arrive after a switch."""
        prompts = [p for p in self._prompts.get(device_id) or [] if not p.expired()]
        self._prompts[device_id] = prompts
        return device_id in self._turns or bool(prompts)

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
        profile = self._face_from[session.device_id] = self._face_profile(session)
        if profile is None:
            return None
        try:
            if profile == "default":  # this gateway's own home, for a standalone profile too
                from hermes_constants import get_hermes_home
                home = get_hermes_home()
            else:
                from hermes_cli.profiles import get_profile_dir
                home = get_profile_dir(profile)
            found = await asyncio.to_thread(faces.for_profile, home, self._convert_avatars)
            if found is None or profile == "default":  # the default profile's approval is session.paired
                return found
            # Found while the profile approved the device: a revocation since then makes it stale.
            return replace(found, valid=lambda: self._face_profile(session) == profile)
        except Exception as exc:  # an unreadable picture shows the mascot; never block a connection on it
            logger.warning("[%s] no face for profile %s: %s", self.name, profile, exc)
            return None

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
            await self._show_prompt(session)  # a question asked while it was away
            await self._push_dashboard(session)
            return
        self._prompts.pop(session.device_id, None)  # unpaired while away: its questions are void
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
        if session.closed or session.paired or self._store.pairing_for(session.device_id):
            return
        await session.send_notice(
            f"No pairing code yet. On the Hermes host run 'hermes pairing list', or allow "
            f"{session.device_id} in GADGET_ALLOWED_USERS.", ttl_s=60)

    async def _watch_pairing(self) -> None:
        """Mirror pairing approvals and revocations onto connected devices."""
        while True:
            await asyncio.sleep(PAIRING_POLL_S)
            if not self._hub:
                continue
            served = None
            for session in list(self._hub.sessions.values()):
                try:  # one device's closed socket must never stop the watcher for the others
                    # Revocation first and with no await before the reset, so nothing can slip in between.
                    if session.profile != "default" and not self._owner_trusts(session.device_id):
                        await self._reset_profile(session)
                    verdict = self._verdict(session)
                    if verdict is None:
                        continue
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
                    elif not verdict and session.paired:
                        self._prompts.pop(session.device_id, None)
                        await session.set_paired(False)
                        await self.on_ready(session)
                    # Another profile approving or revoking the device only shows here: its face follows.
                    if (session.paired and self._profile_faces and session.device_id in self._face_from
                            and self._face_profile(session) != self._face_from[session.device_id]):
                        session.update_face()
                except Exception as exc:
                    logger.debug("[%s] pairing sync failed for %s: %s", self.name, session.device_id, exc)

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
                if s.paired and not s.closed and s.dashboard_version >= dashboard.VERSION]
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
        tiles = self._tiles.tiles()
        for session, switches in zip(targets, switching):
            if not session.paired or (switches and not self._still_trusted(session)):
                continue  # revoked or replaced while the roster was read
            if switches and self._pushed_seq.get(session.device_id) != seq:
                continue  # a newer push has a newer roster for this device and sends it
            # No await from the check to here: send_dashboard queues on the device's send lock (FIFO)
            # before any newer push can, so an older dashboard never lands after a newer one.
            await session.send_dashboard(dashboard.message(
                theirs if switches else mine, tiles, charset=session.charset, cols=session.display.get("text_cols")))

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
        while True:
            await asyncio.sleep(UPDATE_POLL_S)
            if not self._hub:
                continue
            for device_id in queue.pending():
                session = self._hub.get(device_id)
                running = self._installing.get(device_id)
                if session is None or session.closed or (running and not running.done()):
                    continue
                self._installing[device_id] = session.spawn(self._install_staged(queue, session))

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

    async def _forward(self, session: DeviceSession, event: MessageEvent) -> None:
        """Hand a device message to Hermes. Work for another profile needs the default profile's live
        trust and the current connection, so neither a revoked device nor a replaced connection still
        finishing an upload reaches another profile; that profile then checks its own approval."""
        if (event.source.profile not in (None, "default") and self._owner_bound(session)
                and not self._still_trusted(session)):
            logger.info("[%s] dropped a message from %s for profile %s: not trusted by the default profile",
                        self.name, session.device_id, event.source.profile)
            if self._hub is not None and self._hub.get(session.device_id) is session:
                # The device is waiting for an answer; an old connection's leftovers get none.
                await session.turn_end("", "failure")
                await session.send_notice("Not allowed: this gadget is not approved for that profile")
            return
        await self.handle_message(event)

    async def on_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        await self._forward(session, self._event(session, msg_id, text, message_type=MessageType.TEXT))

    async def on_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
        path = await cache_audio_from_bytes_async(wav, ".wav")
        logger.debug("[%s] %.1fs utterance from %s -> %s", self.name, seconds, session.device_id, path)
        await self._forward(session, self._event(
            session, msg_id, "", message_type=MessageType.VOICE, media_urls=[path], media_types=["audio/wav"]))

    async def on_cancel(self, session: DeviceSession) -> None:
        if session.device_id not in self._turns:
            # No turn is running (the reply may still be playing after turn.end): the hub
            # already stopped the audio, and a "/stop" now would start a turn of its own.
            return
        await self._forward(session, self._event(session, "cancel", "/stop", message_type=MessageType.TEXT))

    async def on_new_session(self, session: DeviceSession) -> None:
        self._new_requested[session.device_id] = time.monotonic()
        await self._forward(session, self._event(session, "new", "/new", message_type=MessageType.TEXT))

    async def on_event(self, session: DeviceSession, name: str, data: Any, notify: bool) -> None:
        logger.info("[%s] event from %s: %s %s", self.name, session.device_id, name, data)
        if not notify:
            return
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
        if self._hub and session.device_id not in self._hub.sessions:
            self._turns.pop(session.device_id, None)
            self._pushed_seq.pop(session.device_id, None)
            self._face_from.pop(session.device_id, None)

    # -- turn lifecycle -------------------------------------------------------------

    async def on_processing_start(self, event: MessageEvent) -> None:
        session = self._session(event.source.chat_id)
        if session is None or not session.paired:
            return
        turn = event.message_id or uuid.uuid4().hex[:12]
        self._turns[session.device_id] = turn
        await session.turn_start(turn)

    async def on_processing_complete(self, event: MessageEvent, outcome) -> None:
        turn = self._turns.pop(str(event.source.chat_id), None) or event.message_id or ""
        session = self._session(event.source.chat_id)
        if session is None or not session.paired:
            return
        await session.turn_end(turn, getattr(outcome, "value", str(outcome)))

    def set_status_text(self, chat_id: str, text: Optional[str]) -> None:
        super().set_status_text(chat_id, text)
        session = self._session(chat_id)
        if session is None or self._loop is None:
            return
        coro = session.send_status(text or "")
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
        heard = transcript_echo(content or "")
        if heard is not None:
            # Show what speech-to-text heard as the user's line, not as a reply.
            await session.send_transcript(textfmt.for_device(heard, session.charset))
            return SendResult(success=True, message_id=uuid.uuid4().hex[:12])
        text = textfmt.for_device(content, session.charset)
        if text:
            interim = bool((metadata or {}).get("_interim_send"))
            await session.send_reply(text, turn=self._turns.get(session.device_id), interim=interim)
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    def supports_draft_streaming(self, chat_type=None, metadata=None, chat_id=None) -> bool:
        return True

    async def send_draft(self, chat_id: str, draft_id: int, content: str,
                         metadata: Optional[Dict[str, Any]] = None) -> SendResult:
        session = self._session(chat_id)
        if session is None or not session.paired:
            return SendResult(success=False, error="gadget not connected")
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
            if text:
                await session.send_notice(text, ttl_s=15)
            return SendResult(success=True, message_id=message_id)
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
                                            clarify_id=clarify_id, session_key=session_key, metadata=metadata)
        session = self._session(chat_id)
        if result.success and session is not None and session.paired and self._should_auto_tts_for_chat(chat_id):
            session.spawn(self._speak(chat_id, spoken_question(question, choices)))
        return result

    async def _speak(self, chat_id: str, text: str) -> None:
        """Speak ``text`` on the device with Hermes's configured text-to-speech."""
        paths: List[str] = []
        try:
            from tools.tts_tool import text_to_speech_tool

            raw = await asyncio.to_thread(text_to_speech_tool, text=text)
            result = json.loads(raw) if raw else {}
            paths = [str(p) for p in (result.get("file_paths") or [result.get("file_path")]) if p]
            if not result.get("success") or not paths:
                logger.warning("[%s] could not speak a question: %s", self.name, result.get("error"))
                return
            for path in paths:
                await self._play_file(chat_id, path)
        except Exception as exc:  # TTS unavailable or misconfigured: the text still went out
            logger.warning("[%s] could not speak a question: %s", self.name, exc)
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

    async def _play_file(self, chat_id: str, path: str) -> SendResult:
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
        await session.play_pcm(pcm, session.speaker_rate, turn=self._turns.get(session.device_id))
        return SendResult(success=True, message_id=uuid.uuid4().hex[:12])

    async def play_tts(self, chat_id: str, audio_path: str, **kwargs) -> SendResult:
        return await self._play_file(chat_id, audio_path)

    async def send_voice(self, chat_id: str, audio_path: str, caption: Optional[str] = None,
                         reply_to: Optional[str] = None, metadata: Optional[Dict[str, Any]] = None,
                         **kwargs) -> SendResult:
        result = await self._play_file(chat_id, audio_path)
        if caption:
            await self.send(chat_id, caption, metadata=metadata)
        return result

    def supports_streaming_tts(self, chat_id: str, audio_format: AudioFormat) -> bool:
        session = self._session(chat_id)
        return bool(session and session.paired and session.has_speaker
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
        box = session.image_box
        if box is None or not imaging.pillow_available():
            await session.send_notice("This gadget cannot display images")
        else:
            img = await asyncio.to_thread(imaging.to_rgb565, image_path, box[0], box[1])
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
