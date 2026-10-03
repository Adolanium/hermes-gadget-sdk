"""Interactive simulator window (Tkinter, ships with Python)."""

from __future__ import annotations

import base64
import time
import tkinter as tk
from tkinter import filedialog, ttk

from .. import png
from .runner import Simulator

_LED_COLORS = {
    "off": "#202830", "red": "#f25f5c", "green": "#3dd68c", "blue": "#5aa9f2", "yellow": "#f2c94c",
    "orange": "#f2a03d", "purple": "#a77bf2", "white": "#f0f0f0", "pink": "#f28bd1", "cyan": "#4ce0e8",
}

KEYS_HELP = "SPACE hold = talk   ESC = cancel (hold 2 s: new session)   questions: SPACE = yes, ESC = no   Up/Down = scroll   Ctrl+S = screenshot"


class SimulatorWindow:
    def __init__(self, sim: Simulator, zoom: int = 2):
        self.sim = sim
        self.zoom = zoom
        self.root = tk.Tk()
        self.root.title(f"Hermes Gadget simulator - {sim.board.name}")
        self.root.configure(bg="#11161d")
        self._held: set[str] = set()
        self._release_jobs: dict[str, str] = {}
        self._last_sensor = time.monotonic()
        self._rgb = bytearray(sim.rgb888())
        self._build()
        sim.on_log = self._on_log
        sim.peripherals.changed = self._refresh_peripherals

    # -- layout -------------------------------------------------------------------------

    def _build(self) -> None:
        b = self.sim.board
        left = tk.Frame(self.root, bg="#1c232c", padx=14, pady=14)
        left.grid(row=0, column=0, sticky="ns", padx=10, pady=10)
        self.screen = tk.Label(left, bg="#000000", bd=0)
        self.screen.pack()
        self._photo = None
        self._redraw(0, b.height)

        buttons = tk.Frame(left, bg="#1c232c", pady=10)
        buttons.pack(fill="x")
        talk = tk.Button(buttons, text="TALK (hold)", width=12, bg="#f2b33d", activebackground="#ffd27a")
        talk.bind("<ButtonPress-1>", lambda e: self._button("talk", True))
        talk.bind("<ButtonRelease-1>", lambda e: self._button("talk", False))
        talk.grid(row=0, column=0, padx=3)
        cancel = tk.Button(buttons, text="CANCEL", width=8)
        cancel.bind("<ButtonPress-1>", lambda e: self._button("cancel", True))
        cancel.bind("<ButtonRelease-1>", lambda e: self._button("cancel", False))
        cancel.grid(row=0, column=1, padx=3)
        tk.Button(buttons, text="UP", width=5, command=lambda: self.sim.tap("up")).grid(row=0, column=2, padx=3)
        tk.Button(buttons, text="DOWN", width=5, command=lambda: self.sim.tap("down")).grid(row=0, column=3, padx=3)
        tk.Label(left, text=KEYS_HELP, fg="#8a97a3", bg="#1c232c", font=("Segoe UI", 8)).pack()

        right = tk.Frame(self.root, bg="#11161d")
        right.grid(row=0, column=1, sticky="nsew", padx=(0, 10), pady=10)
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)

        status = ttk.LabelFrame(right, text="Device")
        status.pack(fill="x")
        self.status_var = tk.StringVar()
        tk.Label(status, textvariable=self.status_var, justify="left", anchor="w", font=("Consolas", 9)).pack(fill="x")
        row = tk.Frame(status)
        row.pack(fill="x", pady=4)
        tk.Label(row, text="LED").pack(side="left", padx=(4, 2))
        self.led = tk.Canvas(row, width=22, height=22, highlightthickness=0)
        self.led_dot = self.led.create_oval(3, 3, 19, 19, fill=_LED_COLORS["off"], outline="#000")
        self.led.pack(side="left")
        self.periph_var = tk.StringVar()
        tk.Label(row, textvariable=self.periph_var).pack(side="left", padx=8)
        self.net_var = tk.BooleanVar(value=True)
        tk.Checkbutton(row, text="Wi-Fi", variable=self.net_var,
                       command=lambda: self.sim.set_network(self.net_var.get())).pack(side="right")
        tk.Button(row, text="Reconnect", command=lambda: self.sim.console("reconnect")).pack(side="right", padx=4)

        inputs = ttk.LabelFrame(right, text="Input")
        inputs.pack(fill="x", pady=6)
        self.text_entry = tk.Entry(inputs)
        self.text_entry.pack(side="left", fill="x", expand=True, padx=4, pady=4)
        self.text_entry.bind("<Return>", lambda e: self._send_text())
        tk.Button(inputs, text="Send text", command=self._send_text).pack(side="left", padx=2)
        tk.Button(inputs, text="Speak WAV...", command=self._speak_wav).pack(side="left", padx=2)
        mic_note = "live PC microphone" if self.sim.mic.live else "silence (use Speak WAV, or --live-audio)"
        tk.Label(right, text=f"Holding TALK records: {mic_note}", fg="#8a97a3", bg="#11161d").pack(anchor="w")

        sensors = ttk.LabelFrame(right, text="Sensors")
        sensors.pack(fill="x", pady=6)
        self.battery = tk.Scale(sensors, from_=0, to=100, orient="horizontal", label="battery %",
                                command=lambda v: self.sim.set_sensor("battery_pct", float(v)))
        self.battery.set(round(self.sim.peripherals.battery))
        self.battery.pack(side="left", fill="x", expand=True)
        self.temp = tk.Scale(sensors, from_=-10, to=45, resolution=0.5, orient="horizontal", label="temperature C",
                             command=lambda v: self.sim.set_sensor("temperature_c", float(v)))
        self.temp.set(self.sim.peripherals.temperature_c)
        self.temp.pack(side="left", fill="x", expand=True)
        tk.Button(sensors, text="Event: button.long_press",
                  command=lambda: self.sim.device.emit_event("button.long_press", {"button": "aux"}, False)
                  ).pack(side="left", padx=4)

        console = ttk.LabelFrame(right, text="Serial console")
        console.pack(fill="x", pady=6)
        self.console_entry = tk.Entry(console)
        self.console_entry.pack(fill="x", padx=4, pady=4)
        self.console_entry.insert(0, "help")
        self.console_entry.bind("<Return>", lambda e: self._console())

        logs = ttk.LabelFrame(right, text="Log")
        logs.pack(fill="both", expand=True)
        self.log = tk.Text(logs, height=14, width=70, bg="#0b0f14", fg="#c8d2dc", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)

        self.root.bind("<KeyPress-space>", lambda e: self._key("talk", True))
        self.root.bind("<KeyRelease-space>", lambda e: self._key("talk", False))
        self.root.bind("<KeyPress-Escape>", lambda e: self._key("cancel", True, always=True))
        self.root.bind("<KeyRelease-Escape>", lambda e: self._key("cancel", False, always=True))
        self.root.bind("<Up>", lambda e: self.sim.tap("up"))
        self.root.bind("<Down>", lambda e: self.sim.tap("down"))
        self.root.bind("<Control-s>", lambda e: self._screenshot())
        self.root.protocol("WM_DELETE_WINDOW", self._quit)

    # -- interactions ---------------------------------------------------------------------

    def _focus_is_entry(self) -> bool:
        return isinstance(self.root.focus_get(), tk.Entry)

    def _button(self, name: str, down: bool) -> None:
        """Press/release a device button once per physical press (hold-aware)."""
        if down == (name in self._held):
            return
        if down:
            self._held.add(name)
            self.sim.press(name)
        else:
            self._held.discard(name)
            self.sim.release(name)

    def _key(self, name: str, down: bool, always: bool = False) -> None:
        # Keyboard auto-repeat sends extra presses (Windows) or release+press pairs (X11);
        # a short release delay folds both into one long hold.
        if not always and self._focus_is_entry():
            return
        if down:
            job = self._release_jobs.pop(name, None)
            if job is not None:
                self.root.after_cancel(job)
            self._button(name, True)
        else:
            self._release_jobs[name] = self.root.after(40, lambda: self._key_release(name))

    def _key_release(self, name: str) -> None:
        self._release_jobs.pop(name, None)
        self._button(name, False)

    def _send_text(self) -> None:
        text = self.text_entry.get().strip()
        if text:
            self.sim.type_text(text)
            self.text_entry.delete(0, "end")
            self._append(f"> {text}")

    def _speak_wav(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("WAV audio", "*.wav"), ("All files", "*.*")])
        if path:
            seconds = self.sim.speak_wav(path)
            self._append(f"speaking {path} ({seconds:.1f}s)")

    def _console(self) -> None:
        line = self.console_entry.get().strip()
        if line:
            self._append(f"$ {line}\n{self.sim.console(line)}")
            self.console_entry.delete(0, "end")

    def _screenshot(self) -> None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        base = self.sim.state_dir or __import__("pathlib").Path(".")
        path = self.sim.screenshot(base / "screenshots" / f"screen-{stamp}.png")
        self._append(f"saved {path}")

    def _on_log(self, level: int, message: str) -> None:
        self._append(("DBG ", "INFO", "WARN", "ERR ")[min(level, 3)] + " " + message)

    def _append(self, line: str) -> None:
        self.log.insert("end", line + "\n")
        self.log.see("end")

    def _refresh_peripherals(self) -> None:
        p = self.sim.peripherals
        self.led.itemconfigure(self.led_dot, fill=_LED_COLORS.get(p.led, p.led if p.led.startswith("#") else "#888"))
        self.periph_var.set(f"buzzer beeps: {p.buzzer_count}   backlight: {p.backlight}%   volume: {p.volume}%")

    def _quit(self) -> None:
        self.sim.close()
        self.root.destroy()

    # -- rendering --------------------------------------------------------------------------

    def _redraw(self, y0: int, y1: int) -> None:
        b = self.sim.board
        stride = b.width * 3
        self._rgb[y0 * stride:y1 * stride] = self.sim.rgb888(y0, y1)
        ppm = png.encode_ppm(bytes(self._rgb), b.width, b.height)
        try:
            img = tk.PhotoImage(data=ppm, format="PPM")
        except tk.TclError:
            img = tk.PhotoImage(data=base64.b64encode(png.encode_png(bytes(self._rgb), b.width, b.height)))
        if self.zoom > 1:
            img = img.zoom(self.zoom)
        self._photo = img
        self.screen.configure(image=img)

    def _tick(self) -> None:
        self.sim.step()
        rows = self.sim.take_dirty_rows()
        if rows:
            self._redraw(*rows)
        st = self.sim.status()
        self.status_var.set(
            f"id      {st.get('device_id')}\nscreen  {st.get('screen'):<11} phase {st.get('phase')}\n"
            f"paired  {st.get('paired')}   server {st.get('server')}")
        if time.monotonic() - self._last_sensor > 30:
            self._last_sensor = time.monotonic()
            self.sim.drift_sensors()
        self.root.after(15, self._tick)

    def run(self) -> None:
        self.sim.start(network=True)
        self._refresh_peripherals()
        self._append("Simulator started. " + KEYS_HELP)
        self.root.after(15, self._tick)
        self.root.mainloop()
