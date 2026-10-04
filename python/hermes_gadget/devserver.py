"""Standalone development server: the real device hub with a scripted "brain".

Use it to develop firmware and boards without a Hermes install or any model
calls. It runs ``hermes_gadget_plugin.hub`` — the same code the Hermes gateway
runs — with a delegate that:

* echoes typed text back, streaming it word by word like a model would;
* answers voice with a short report and plays the recording back
  (a microphone -> server -> speaker loopback test);
* simulates pairing codes, approved from this server's console;
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
from hermes_gadget_plugin import textfmt
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
    def __init__(self, *, require_pairing: bool = False, loopback: bool = True, word_delay: float = 0.08):
        self.require_pairing = require_pairing
        self.loopback = loopback
        self.word_delay = word_delay
        self.approved: set[str] = set()
        self.codes: dict[str, str] = {}  # code -> device_id
        self._turns: dict[str, asyncio.Task] = {}
        self.hub: DeviceHub | None = None

    async def is_paired(self, session: DeviceSession) -> bool:
        return not self.require_pairing or session.device_id in self.approved

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

    async def _turn(self, session: DeviceSession, turn: str, reply: str, audio: bytes | None) -> None:
        await session.turn_start(turn)
        try:
            await session.send_status("Thinking")
            await asyncio.sleep(0.4)
            if audio and session.has_speaker:
                await session.play_pcm(audio, session.speaker_rate, turn=turn)
            words = reply.split(" ")
            for i in range(1, len(words) + 1):
                await session.send_delta(" ".join(words[:i]), turn=turn)
                await asyncio.sleep(self.word_delay)
            await session.send_reply(reply, turn=turn)
            await session.turn_end(turn, "success")
        except asyncio.CancelledError:
            if not session.closed:  # a stopping server has nobody left to tell
                await session.stop_audio()
                await session.turn_end(turn, "cancelled")
            raise

    def _start_turn(self, session: DeviceSession, msg_id: str, reply: str, audio: bytes | None) -> None:
        previous = self._turns.get(session.device_id)
        if previous and not previous.done():
            previous.cancel()
        turn = f"{session.session_id}:{msg_id}"
        self._turns[session.device_id] = session.spawn(self._turn(session, turn, reply, audio))

    async def on_text(self, session: DeviceSession, msg_id: str, text: str) -> None:
        reply = textfmt.for_device(f"You said: {text}", session.charset)
        audio = tone(session.speaker_rate, 0.25) if session.has_speaker else None
        self._start_turn(session, msg_id, reply, audio)

    async def on_utterance(self, session: DeviceSession, msg_id: str, wav: bytes, seconds: float) -> None:
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
  notice <text>                show a notice line
  ask <title> | <question>     ask a yes/no question (TALK = yes, CANCEL = no)
  action <name> [json-args]    invoke a device action, e.g. action led.set {"color":"red"}
  image <path>                 send an image (needs Pillow)
  update <path>                install a firmware image (firmware.bin) over the air
  quit"""


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
            elif cmd == "update":
                for s in sessions:
                    s.spawn(_update(s, rest.strip()))
            elif cmd == "quit":
                stop.set()
            else:
                print("unknown command; type help")
        except Exception as exc:
            print(f"  error: {exc}")


async def serve(host: str, port: int, path: str, state_dir: Path, *, require_pairing: bool,
                loopback: bool, token: str | None, interactive: bool = True,
                ready: "asyncio.Future | None" = None) -> None:
    brain = EchoBrain(require_pairing=require_pairing, loopback=loopback)
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
