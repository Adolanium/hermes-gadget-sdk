"""Standalone development server: the real device hub with a scripted "brain".

Use it to develop firmware and boards without a Hermes install or any model
calls. It runs ``hermes_gadget_plugin.hub`` — the same code the Hermes gateway
runs — with a delegate that:

* echoes typed text back, streaming it word by word like a model would;
* answers voice with a short report and plays the recording back
  (a microphone -> server -> speaker loopback test);
* simulates pairing codes, approved from this server's console;
* runs interpreter mode with a stand-in for the translation (``interpret es en``);
* lets you push cards, notices, questions, images and action calls to a
  device from the console (type ``help``).
"""

from __future__ import annotations

import asyncio
import logging
import math
import secrets
import struct
import sys
import tempfile
import threading
from pathlib import Path

from hermes_gadget_plugin import audio as pcm_tools
from hermes_gadget_plugin import interpret, textfmt
from hermes_gadget_plugin.hub import DeviceHub, DeviceSession, HubDelegate
from hermes_gadget_plugin.store import DeviceStore

log = logging.getLogger("hermes_gadget.devserver")

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def tone(rate: int, seconds: float, freq: float = 660.0, volume: float = 0.25) -> bytes:
    n = int(rate * seconds)
    fade = max(1, int(rate * 0.01))
    out = bytearray()
    for i in range(n):
        env = min(1.0, i / fade, (n - i) / fade)
        out += struct.pack("<h", int(32767 * volume * env * math.sin(2 * math.pi * freq * i / rate)))
    return bytes(out)


class EchoBrain(HubDelegate):
    def __init__(self, *, require_pairing: bool = False, loopback: bool = True, word_delay: float = 0.08,
                 language: str | None = None):
        self.require_pairing = require_pairing
        self.language_id = language  # named in welcome, as Hermes's language is
        self.loopback = loopback
        self.word_delay = word_delay
        self.approved: set[str] = set()
        self.codes: dict[str, str] = {}  # code -> device_id
        self._turns: dict[str, asyncio.Task] = {}
        self.hub: DeviceHub | None = None
        # Interpreter mode, as the Hermes adapter runs it (plugin/interpret.py), with stand-ins for what
        # Hermes does: `transcribe(seconds, language)` for speech-to-text and `translate(text, session)`
        # for the model. The defaults only label the text, so the screens can be developed without a model.
        self.interpreting: dict[str, interpret.Session] = {}
        self.transcribe = lambda seconds, language: f"{seconds:.1f} s of {language} speech"
        self.translate = lambda text, session: f"[{session.label}] {text}"

    async def is_paired(self, session: DeviceSession) -> bool:
        return not self.require_pairing or session.device_id in self.approved

    def language(self, session: DeviceSession) -> str | None:
        return self.language_id

    async def on_ready(self, session: DeviceSession) -> None:
        if session.paired:
            return
        code = next((c for c, d in self.codes.items() if d == session.device_id), None)
        if code is None:
            code = "".join(secrets.choice(ALPHABET) for _ in range(8))
            self.codes[code] = session.device_id
        command = f"approve {code}   (type this in the devserver console)"
        print(f"\n>>> {session.name} ({session.device_id}) wants to pair. Code: {code}  ->  approve {code}")
        await session.send_pairing(code, command)

    async def approve(self, code: str) -> bool:
        device_id = self.codes.pop(code.upper(), None)
        if device_id is None:
            return False
        self.approved.add(device_id)
        session = self.hub.get(device_id) if self.hub else None
        if session:
            await session.set_paired(True)
        return True

    def interprets(self, session: DeviceSession) -> bool:
        return session.device_id in self.interpreting

    async def _stop_turn(self, session: DeviceSession) -> None:
        """Stop the device's turn under way, an answer or a translation, and wait until it has ended."""
        task = self._turns.get(session.device_id)
        if task and not task.done() and task is not asyncio.current_task():
            task.cancel()
            await asyncio.wait({task})

    async def interpret(self, session: DeviceSession, first: str, second: str) -> interpret.Session:
        state = self.interpreting[session.device_id] = interpret.start(first, second)
        # As the Hermes adapter does: an answer under way is not for whoever holds the gadget next,
        # a translation of the mode before is not this mode's, and a choice on screen goes unanswered.
        await self._stop_turn(session)
        await session.withdraw_choice(f"{session.name} now interprets, and takes no questions")
        await session.send_interpret(state.message())
        return state

    async def interpret_off(self, session: DeviceSession) -> bool:
        if self.interpreting.pop(session.device_id, None) is None:
            return False
        await self._stop_turn(session)  # no more of a translation arrives after the mode is off
        await session.send_interpret(interpret.OFF)
        return True

    async def on_interpret_flip(self, session: DeviceSession) -> None:
        state = self.interpreting.get(session.device_id)
        task = self._turns.get(session.device_id)
        if state is None or (task and not task.done()):
            return
        state.flip()
        await session.send_interpret(state.message())

    async def _turn(self, session: DeviceSession, turn: str, reply: str, audio: bytes | None,
                    heard: str | None = None, interpreting: interpret.Session | None = None) -> None:
        await session.turn_start(turn)
        try:
            if heard is not None:
                await session.send_transcript(textfmt.for_device(heard, session.charset))
            await session.send_status("Thinking")
            await asyncio.sleep(0.4)
            if audio and session.has_speaker:
                await session.play_pcm(audio, session.speaker_rate, turn=turn)
            words = reply.split(" ")
            for i in range(1, len(words) + 1):
                await session.send_delta(" ".join(words[:i]), turn=turn)
                await asyncio.sleep(self.word_delay)
            await session.send_reply(reply, turn=turn)
            if interpreting is not None and self.interpreting.get(session.device_id) is interpreting:
                interpreting.flip()  # the gadget changes hands
                await session.send_interpret(interpreting.message())
            await session.turn_end(turn, "success")
        except asyncio.CancelledError:
            if not session.closed:  # a stopping server has nobody left to tell
                await session.stop_audio()
                await session.turn_end(turn, "cancelled")
            raise

    def _start_turn(self, session: DeviceSession, msg_id: str, reply: str, audio: bytes | None, **kw) -> None:
        previous = self._turns.get(session.device_id)
        if previous and not previous.done():
            previous.cancel()
        turn = f"{session.session_id}:{msg_id}"
        self._turns[session.device_id] = session.spawn(self._turn(session, turn, reply, audio, **kw))

    def _interpret_turn(self, session: DeviceSession, msg_id: str, state: interpret.Session, said: str,
                        heard: str | None) -> None:
        """A translation in place of an answer: the stand-in's text, as the device can show it, and a tone."""
        reply = interpret.shown(self.translate(said, state), session.charset, state.target,
                                spoken=session.has_speaker)
        audio = tone(session.speaker_rate, 0.25) if session.has_speaker else None  # stands in for the speech
        self._start_turn(session, msg_id, reply, audio, heard=heard, interpreting=state)

    # A device marks what is typed or recorded in the mode; that is translated or refused, never echoed.

    async def on_interpret_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        if (state := self.interpreting.get(session.device_id)) is None:
            await session.refuse_interpreted()
            return
        self._interpret_turn(session, msg_id, state, text, None)

    async def on_interpret_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
        if (state := self.interpreting.get(session.device_id)) is None:
            await session.refuse_interpreted()
            return
        heard = self.transcribe(seconds, state.speaker)
        self._interpret_turn(session, msg_id, state, heard, heard)

    async def on_interpret_stop(self, session: DeviceSession) -> None:
        self.interpreting.pop(session.device_id, None)
        await self._stop_turn(session)
        await session.refuse_interpreted("Interpreter off")

    async def on_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        if (state := self.interpreting.get(session.device_id)) is not None:
            self._interpret_turn(session, msg_id, state, text, None)
            return
        reply = textfmt.for_device(f"You said: {text}", session.charset)
        audio = tone(session.speaker_rate, 0.25) if session.has_speaker else None
        self._start_turn(session, msg_id, reply, audio)

    async def on_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
        if (state := self.interpreting.get(session.device_id)) is not None:
            heard = self.transcribe(seconds, state.speaker)
            self._interpret_turn(session, msg_id, state, heard, heard)
            return
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            f.write(wav)
            path = f.name
        pcm = pcm_tools.decode_file(path, session.speaker_rate)
        Path(path).unlink(missing_ok=True)
        peak = max((abs(v) for (v,) in struct.iter_unpack("<h", pcm)), default=0)
        db = 20 * math.log10(peak / 32768) if peak else -96.0
        reply = (f"I heard {seconds:.1f} seconds of audio (peak {db:.0f} dBFS). "
                 + ("Playing it back." if self.loopback and session.has_speaker else ""))
        self._start_turn(session, msg_id, reply.strip(), pcm if self.loopback else None)

    async def on_new_session(self, session: DeviceSession) -> None:
        if await self.interpret_off(session):  # holding CANCEL leaves interpreter mode
            await session.turn_end("", "cancelled")
            await session.send_notice("Interpreter off")
            return
        self._start_turn(session, "new", "Started a new conversation.", None)

    async def on_prompt_reply(self, session: DeviceSession, prompt_id: str, yes: bool) -> None:
        print(f"[answer] {session.name}: {prompt_id} -> {'yes' if yes else 'no'}")
        await session.send_notice(f"You answered {'yes' if yes else 'no'}")

    async def on_cancel(self, session: DeviceSession) -> None:
        task = self._turns.get(session.device_id)
        if task and not task.done():
            task.cancel()

    async def on_event(self, session: DeviceSession, name: str, data, notify: bool) -> None:
        print(f"[event] {session.name}: {name} {data} (notify={notify})")

    async def on_state(self, session: DeviceSession, sensors: dict) -> None:
        log.info("[state] %s: %s", session.name, sensors)


HELP = """commands:
  list                         connected devices
  approve <CODE>               approve a pairing code
  say <text>                   push a reply to every device
  card <title> | <body>        show a card
  show <json>                  show a structured card, e.g.
                               show {"title":"Build","progress":40,"items":["core","sim"],"id":"b"}
                               show {"title":"Join","qr":"https://example.org"}
  choose <title> | <a> | <b>.. ask for one of 2 to 6 answers and print the one picked
  notice <text>                show a notice line
  ask <title> | <question>     ask a yes/no question (TALK = yes, CANCEL = no)
  action <name> [json-args]    invoke a device action, e.g. action led.set {"color":"red"}
  image <path>                 send an image (needs Pillow)
  dashboard [json]             send a dashboard (sample sessions, tiles and widgets without json);
                               CANCEL on the idle screen opens the list, DOWN or a swipe
                               from left to right the widgets
  play <path> | stop           play a sound file on every speaker (WAV; other formats need ffmpeg)
  interpret <a> <b>            interpreter mode between two languages, e.g. interpret es en;
                               the translation is a stand-in that labels the text
  interpret off                leave interpreter mode
  update <path>                install a firmware image (firmware.bin) over the air
  quit"""


# What `dashboard` sends without arguments: a few sessions in each state, two tiles and one widget of each kind.
SAMPLE_DASHBOARD = {
    "sessions": [{"label": "telegram 3f2a", "state": "waiting"}, {"label": "gadget 91c0", "state": "busy"},
                 {"label": "cli 77de", "state": "idle"}],
    "tiles": [{"label": "NAS", "value": "up", "state": "ok"},
              {"label": "Backups", "value": "2 days", "state": "warn"}],
    "widgets": [{"id": "office", "kind": "value", "label": "Office", "value": "21.5", "unit": "C", "state": "ok"},
                {"id": "backup", "kind": "progress", "label": "Backup", "percent": 62},
                {"id": "today", "kind": "list", "label": "Today",
                 "items": ["09:00 Standup", "11:30 Dentist", "14:00 Review", "18:00 Gym"]},
                {"id": "playing", "kind": "headline", "label": "Now playing", "title": "Blue in Green",
                 "text": "Miles Davis"}],
}


async def _update(session: DeviceSession, path: str) -> None:
    from hermes_gadget_plugin.ota import UpdateError

    shown = -1

    def progress(sent: int, total: int) -> None:
        nonlocal shown
        pct = sent * 100 // max(1, total)
        if pct >= shown + 10 or sent == total:
            shown = pct
            print(f"  {session.name}: {pct}%")

    try:
        version = await session.update_firmware(Path(path).read_bytes(), progress=progress)
    except UpdateError as exc:
        print(f"  {session.name}: update failed: {exc.message}")
        return
    print(f"  {session.name}: installed {version}; it restarts now")


async def _console(hub: DeviceHub, brain: EchoBrain, stop: asyncio.Event) -> None:
    import json

    loop = asyncio.get_running_loop()
    lines: asyncio.Queue[str | None] = asyncio.Queue()

    def reader() -> None:
        for line in sys.stdin:
            loop.call_soon_threadsafe(lines.put_nowait, line)
        loop.call_soon_threadsafe(lines.put_nowait, None)

    threading.Thread(target=reader, daemon=True).start()
    while not stop.is_set():
        line = await lines.get()
        if line is None:
            return
        cmd, _, rest = line.strip().partition(" ")
        sessions = list(hub.sessions.values())
        try:
            if cmd in ("", "#"):
                continue
            if cmd == "help":
                print(HELP)
            elif cmd == "list":
                for s in sessions:
                    print(f"  {s.device_id} {s.name!r} board={s.board} paired={s.paired} "
                          f"actions={s.action_names()} sensors={s.sensors}")
                if not sessions:
                    print("  (no devices connected)")
            elif cmd == "approve":
                print("approved" if await brain.approve(rest.strip()) else "unknown code")
            elif cmd == "say":
                for s in sessions:
                    await s.send_reply(rest)
            elif cmd == "card":
                title, _, body = rest.partition("|")
                for s in sessions:
                    await s.show_card(title.strip(), body.strip())
            elif cmd == "show":
                given = json.loads(rest)
                for s in sessions:
                    await s.show_card(
                        str(given.get("title") or ""), str(given.get("body") or ""), given.get("seconds", 0),
                        items=[str(i) for i in given.get("items") or []], progress=given.get("progress"),
                        qr=given.get("qr"), card_id=given.get("id"))
            elif cmd == "choose":
                title, *options = [part.strip() for part in rest.split("|")]
                for s in sessions:
                    s.spawn(_choose(s, title, options))
            elif cmd == "notice":
                for s in sessions:
                    await s.send_notice(rest)
            elif cmd == "ask":
                title, _, question = rest.partition("|")
                for s in sessions:
                    await s.ask(f"q{secrets.token_hex(3)}", title.strip(), question.strip())
            elif cmd == "action":
                name, _, raw = rest.partition(" ")
                args = json.loads(raw) if raw.strip() else {}
                for s in sessions:
                    print(f"  {s.name}: {await s.invoke_action(name, args)}")
            elif cmd == "image":
                from hermes_gadget_plugin import imaging

                for s in sessions:
                    box = s.image_box
                    if box:
                        img = imaging.to_rgb565(rest.strip(), *box)
                        await s.show_image(img.width, img.height, img.rgb565)
            elif cmd == "dashboard":
                from hermes_gadget_plugin import dashboard

                given = json.loads(rest) if rest.strip() else SAMPLE_DASHBOARD
                for s in sessions:
                    if s.dashboard_version:
                        await s.send_dashboard(dashboard.message(
                            given.get("sessions") or [], given.get("tiles") or [], given.get("widgets") or [],
                            charset=s.charset, cols=s.display.get("text_cols"), version=s.dashboard_version))
            elif cmd in ("play", "stop"):
                from hermes_gadget_plugin import playback

                for s in sessions:
                    if cmd == "stop":
                        await playback.stop(s)
                    elif s.has_speaker:
                        path = Path(rest.strip()).expanduser()  # the developer's own file: any folder
                        # Resolving reads the disk too: both off the loop that serves the devices.
                        source = await asyncio.to_thread(lambda p=path, r=s.speaker_rate: playback.open_source(
                            str(p), r, roots=[p.resolve().parent]))
                        await playback.play(s, source, title=path.name, max_s=playback.max_seconds({}))
            elif cmd == "interpret":
                words = rest.split()
                for s in sessions:
                    if words == ["off"]:
                        await brain.interpret_off(s)
                    elif len(words) == 2:
                        state = await brain.interpret(s, words[0], words[1])
                        print(f"  {s.name}: {state.label}")
                    else:
                        print("  usage: interpret <a> <b> | interpret off")
            elif cmd == "update":
                for s in sessions:
                    s.spawn(_update(s, rest.strip()))
            elif cmd == "quit":
                stop.set()
            else:
                print("unknown command; type help")
        except Exception as exc:
            print(f"  error: {exc}")


async def _choose(session, title: str, options: list[str]) -> None:
    """Asks one device and prints what it answered; the console stays free meanwhile."""
    try:
        index = await session.ask_choice(title, options)
        print(f"  {session.name}: " + ("cancelled" if index is None else options[index]))
    except Exception as exc:
        print(f"  {session.name}: {exc}")


async def serve(host: str, port: int, path: str, state_dir: Path, *, require_pairing: bool,
                loopback: bool, token: str | None, interactive: bool = True,
                ready: "asyncio.Future | None" = None, language: str | None = None) -> None:
    brain = EchoBrain(require_pairing=require_pairing, loopback=loopback, language=language)
    hub = DeviceHub(DeviceStore(state_dir), brain, host=host, port=port, path=path, access_token=token)
    brain.hub = hub
    await hub.start()
    print(f"Hermes Gadget dev server on ws://{host}:{hub.bound_port}{hub.path}  (type 'help')")
    if ready is not None and not ready.done():
        ready.set_result(hub)
    stop = asyncio.Event()
    try:
        if interactive:
            console = asyncio.create_task(_console(hub, brain, stop))
            await stop.wait()
            console.cancel()
        else:
            await stop.wait()
    finally:
        await hub.stop()
