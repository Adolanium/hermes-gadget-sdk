"""Screenshot the real simulator window for the README (and record it for videos).

    python tools/capture_window.py [--board sim-466x466-round] [--out docs/images] [--frames DIR]

Opens the actual `hermes-gadget sim` window against the development hub, plays
a short scripted session (pairing, a voice turn, a card, a question) on the
window's own thread, and grabs what is on screen:

    sim-window.png      the whole simulator window
    screen-*.png        the device screen in each state

With --frames, the device panel (screen and buttons) is also recorded and saved
per scene, for making videos.
Needs Pillow and a desktop session (the window must be visible while it runs).
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import math
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

from PIL import Image, ImageGrab

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "python"))
sys.path.insert(0, str(REPO / "tools"))

from hermes_gadget import devserver  # noqa: E402
from hermes_gadget.sim import Simulator  # noqa: E402
from hermes_gadget.sim.window import SimulatorWindow  # noqa: E402

FPS = 15
QUESTION = "What's the weather in Lisbon tomorrow?"
ANSWER = ("Sunny and 24 degrees in Lisbon tomorrow, with a light breeze in the afternoon. "
          "A good day for the waterfront.")


def tone(seconds: float, freq: float, volume: int, wobble: bool = False) -> bytes:
    out = bytearray()
    for i in range(int(16000 * seconds)):
        env = 0.55 + 0.45 * math.sin(2 * math.pi * 3 * i / 16000) if wobble else 1.0
        out += struct.pack("<h", int(volume * env * math.sin(2 * math.pi * freq * i / 16000)))
    return bytes(out)


class Recorder(SimulatorWindow):
    """The real simulator window, plus a scripted director and screen grabs."""

    def __init__(self, sim, hub, loop, zoom, frames_dir: Path | None):
        super().__init__(sim, zoom=zoom)
        self.hub, self.loop, self.frames_dir = hub, loop, frames_dir
        self.root.attributes("-topmost", True)
        self.scene: str | None = None
        self.scenes: dict[str, list[Image.Image]] = {}
        self.stills: dict[str, Image.Image] = {}
        self._next_grab = 0.0
        self._script = self.script()

    # geometry, in physical pixels (the process is DPI aware)
    def _box(self, widget):
        x, y = widget.winfo_rootx(), widget.winfo_rooty()
        return (x, y, x + widget.winfo_width(), y + widget.winfo_height())

    def grab(self, widget=None) -> Image.Image:
        # Tk repaints in idle time, which a busy timer loop can starve: paint first.
        self.root.update_idletasks()
        if widget is None:
            r = self.root
            return ImageGrab.grab(bbox=(r.winfo_rootx(), r.winfo_rooty(), r.winfo_rootx() + r.winfo_width(),
                                        r.winfo_rooty() + r.winfo_height()), all_screens=True)
        return ImageGrab.grab(bbox=self._box(widget), all_screens=True)

    def run_hub(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(10)

    @property
    def session(self):
        return next(iter(self.hub.sessions.values()))

    def _tick(self) -> None:
        super()._tick()
        now = time.monotonic()
        if self.scene and now >= self._next_grab:
            self._next_grab = now + 1 / FPS
            self.scenes.setdefault(self.scene, []).append(self.grab(self.screen.master))

    def advance(self) -> None:
        try:
            wait = next(self._script)
        except StopIteration:
            self.finish()
            return
        self.root.after(int(wait * 1000), self.advance)

    def script(self):
        sim = self.sim
        yield 1.5
        while not sim.status().get("pairing_code"):
            yield 0.2
        code = sim.status()["pairing_code"]
        self.run_hub(self.session.send_pairing(code, f"hermes pairing approve gadget {code}"))
        self.scene = "pairing"
        yield 0.6
        self.stills["pairing"] = self.grab(self.screen)
        yield 2.4
        if not self.run_hub(self.hub.delegate.approve(code)):
            print(f"pairing code {code} was not accepted; status {sim.status()}", flush=True)
        self.scene = "paired"
        yield 3.5
        self.scene = "idle"
        yield 0.5
        self.stills["ready"] = self.grab(self.screen)
        self.stills["window"] = self.grab()
        yield 2.5

        sim.mic.inject(tone(2.6, 220, 14000, wobble=True))
        sim.press("talk")
        self.scene = "listen"
        yield 1.0
        self.stills["listening"] = self.grab(self.screen)
        yield 1.5
        sim.release("talk")
        session = self.session

        async def think():
            await session.turn_start("v")
            await session.send_transcript(QUESTION)
            await session.send_status("Checking the forecast")

        self.run_hub(think())
        self.scene = "think"
        yield 0.8
        self.stills["thinking"] = self.grab(self.screen)
        yield 1.7
        self.run_hub(session.play_pcm(tone(6.5, 300, 2500), 16000, turn="v"))
        self.scene = "speak"
        yield 0.8
        self.stills["speaking"] = self.grab(self.screen)
        yield 0.2
        words = ANSWER.split(" ")
        for i in range(1, len(words) + 1):
            self.run_hub(session.send_delta(" ".join(words[:i]), turn="v"))
            yield 0.22
        self.run_hub(session.send_reply(ANSWER, turn="v"))
        yield 1.0
        self.stills["reply"] = self.grab(self.screen)
        yield 1.5
        self.run_hub(session.turn_end("v"))
        self.run_hub(session.stop_audio())
        yield 0.5

        self.run_hub(session.show_card("Pasta timer", "Drain at 7:42 pm. Save a cup of the water for the sauce.",
                                       ttl_s=0))
        self.scene = "card"
        yield 0.8
        self.stills["card"] = self.grab(self.screen)
        yield 2.7
        sim.tap("cancel")
        self.run_hub(session.ask("q1", "Allow this command?", "git push --force\nrewrites remote history"))
        self.scene = "question"
        yield 0.8
        self.stills["prompt"] = self.grab(self.screen)
        yield 1.8
        sim.press("talk")
        yield 0.3
        sim.release("talk")
        yield 1.0
        self.scene = None

    def finish(self) -> None:
        self.sim.close()
        self.root.destroy()


def main() -> None:
    global FPS
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="sim-466x466-round")
    ap.add_argument("--zoom", type=int, default=1)
    ap.add_argument("--out", default=str(REPO / "docs" / "images"))
    ap.add_argument("--frames", help="also save every recorded panel frame here, per scene")
    ap.add_argument("--fps", type=int, default=FPS, help="recording rate")
    args = ap.parse_args()
    FPS = args.fps
    try:  # grab real pixels, not scaled window coordinates
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass

    state = Path(tempfile.mkdtemp())
    loop = asyncio.new_event_loop()
    ready = loop.create_future()
    threading.Thread(target=lambda: loop.run_until_complete(devserver.serve(
        "127.0.0.1", 0, "/gadget", state / "srv", require_pairing=True, loopback=False, token=None,
        interactive=False, ready=ready)), daemon=True).start()
    while not ready.done():
        time.sleep(0.05)
    hub = ready.result()

    async def quiet(*_):  # the script plays the agent
        return None

    hub.delegate.on_utterance = quiet
    sim = Simulator(url=f"ws://127.0.0.1:{hub.bound_port}/gadget", state_dir=state / "dev", name="Hermes Gadget",
                    board=args.board)
    win = Recorder(sim, hub, loop, args.zoom, Path(args.frames) if args.frames else None)
    win.root.after(500, win.advance)
    win.run()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    win.stills["window"].save(out / "sim-window.png", optimize=True)
    for name, img in win.stills.items():
        if name != "window":
            img.save(out / f"screen-{name}.png", optimize=True)
    if args.frames:
        for name, frames in win.scenes.items():
            d = Path(args.frames) / name
            d.mkdir(parents=True, exist_ok=True)
            for i, f in enumerate(frames):
                f.save(d / f"{i:04d}.png")
    print("scenes", {k: len(v) for k, v in win.scenes.items()})


if __name__ == "__main__":
    main()
