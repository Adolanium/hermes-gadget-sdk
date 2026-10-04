"""Desktop simulator: the firmware display, conversation, and optional developer tools."""

from __future__ import annotations

import base64
import math
import time
import tkinter as tk
import wave
from pathlib import Path
from tkinter import filedialog, ttk
from urllib.parse import urlsplit

from .. import paths, png
from .audio_io import Speaker
from .conversation import Conversation
from .runner import BOARDS, Simulator

BG = "#141c26"
PANEL = "#1c2633"
INK = "#e6edf3"
MUTED = "#a3b3c3"
AMBER = "#efb451"
LINE = "#394959"
_LED_COLORS = {
    "off": "#202830", "red": "#f25f5c", "green": "#3dd68c", "blue": "#5aa9f2", "yellow": "#f2c94c",
    "orange": "#f2a03d", "purple": "#a77bf2", "white": "#f0f0f0", "pink": "#f28bd1", "cyan": "#4ce0e8",
}
KEYS_HELP = "Hold Space to talk · Esc to cancel · Hold Esc for a new conversation · Ctrl+S to save the screen"


class SimulatorWindow:
    def __init__(self, sim: Simulator, zoom: int = 2):
        self.sim = sim
        self.requested_zoom = max(1, zoom)
        self.root = tk.Tk()
        self.root.configure(bg=BG)
        icon = paths.repo_root() / "docs" / "images" / "logo.png"
        self._icon = None
        if icon.exists():
            self._icon = tk.PhotoImage(file=str(icon), master=self.root)
            self._icon = self._icon.subsample(max(1, math.ceil(self._icon.width() / 32)))
        if self._icon is not None:
            self.root.iconphoto(True, self._icon)
        self.root.option_add("*Font", ("Segoe UI", 10))
        self.root.option_add("*Background", PANEL)
        self.root.option_add("*Foreground", INK)
        self.root.option_add("*insertBackground", INK)
        self.root.option_add("*selectBackground", "#405369")
        self.root.option_add("*selectForeground", INK)
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TCombobox", fieldbackground=BG, background=PANEL, foreground=INK)
        style.map("TCombobox", fieldbackground=[("readonly", BG)], foreground=[("readonly", INK)])
        self._held: set[str] = set()
        self._release_jobs: dict[str, str] = {}
        self._last_sensor = time.monotonic()
        self.history = Conversation()
        self._history_revision = -1
        self._replay = Speaker(None)
        self._settings_window = None
        self._build()
        self._attach()

    def _label(self, parent, text="", **options):
        return tk.Label(parent, text=text, bg=parent.cget("bg"), fg=INK, **options)

    def _action(self, parent, text, command=None, *, primary=False):
        return tk.Button(parent, text=text, command=command, bg=AMBER if primary else PANEL,
                         fg="#242019" if primary else INK, activebackground="#ffd078" if primary else LINE,
                         activeforeground="#242019" if primary else INK, relief="flat", borderwidth=0,
                         padx=15, pady=9, cursor="hand2", highlightthickness=1, highlightbackground=LINE)

    def _entry(self, parent):
        return tk.Entry(parent, bg=BG, fg=INK, insertbackground=INK, relief="flat", borderwidth=0,
                        highlightthickness=1, highlightbackground=LINE, highlightcolor=AMBER)

    def _attach(self):
        self.sim.on_log = self._on_log
        self.sim.on_message = self._message
        self.sim.peripherals.changed = self._refresh_peripherals
        self._refresh_peripherals()

    def _build(self):
        self._history_revision = -1
        b = self.sim.board
        # Prefer integer pixels; use half size when the native panel cannot fit.
        width = min(1220, self.root.winfo_screenwidth() - 60)
        self.zoom = min(self.requested_zoom, max(1, (width - 470) // b.width),
                        max(1, (self.root.winfo_screenheight() - 450) // b.height))
        self._shrink = 2 if b.height + 360 > self.root.winfo_screenheight() - 90 else 1
        w, h = b.width * self.zoom // self._shrink, b.height * self.zoom // self._shrink
        self._rgb = bytearray(self.sim.rgb888())
        self._round_spans = []
        if b.round:
            radius = min(b.width, b.height) / 2
            for y in range(b.height):
                half = math.sqrt(max(0, radius ** 2 - (y + 0.5 - b.height / 2) ** 2))
                self._round_spans.append((max(0, round(b.width / 2 - half)), min(b.width, round(b.width / 2 + half))))
        self.root.title(f"{self.sim.status().get('name', 'Hermes Gadget')} · Simulator")
        self._base_height = min(self.root.winfo_screenheight() - 90, max(720, h + 360))
        self.root.geometry(f"{width}x{self._base_height}")
        self.root.minsize(min(width, w + 470), self._base_height)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        header = tk.Frame(self.root, bg=PANEL, padx=24, pady=12)
        header.grid(row=0, column=0, sticky="ew")
        if self._icon is not None:
            self._label(header, image=self._icon).pack(side="left", padx=(0, 12))
        self._label(header, "Hermes Gadget", font=("Segoe UI", 15, "bold")).pack(side="left")
        self._label(header, "DESKTOP SIMULATOR", font=("Segoe UI", 8)).pack(side="left", padx=18)
        self._action(header, "Settings", self._settings).pack(side="right")
        self.connection_var = tk.StringVar(value="Connecting")
        self.connection_label = self._label(header, textvariable=self.connection_var)
        self.connection_label.pack(side="right", padx=20)

        main = tk.Frame(self.root, bg=BG)
        main.grid(row=1, column=0, sticky="nsew")
        main.columnconfigure(0, weight=1)
        main.columnconfigure(1, minsize=390)
        main.rowconfigure(0, weight=1)
        left = tk.Frame(main, bg=BG, padx=18, pady=12)
        left.grid(row=0, column=0, sticky="nsew")
        self._label(left, f"{b.name} · {b.width} × {b.height}", font=("Segoe UI", 10)).pack(pady=(0, 14))
        bezel = tk.Canvas(left, width=w + 28, height=h + 28, bg=BG, highlightthickness=0)
        bezel.pack(expand=True)
        if not b.round:
            bezel.create_rectangle(2, 2, w + 26, h + 26, fill="#293645", outline=LINE, width=2)
        self.screen = tk.Label(bezel, bg=BG, bd=0)
        bezel.create_window(14, 14, window=self.screen, anchor="nw", width=w, height=h)
        self._photo = None
        self._redraw(0, b.height)
        self.ready_var = tk.StringVar(value="Connect your Hermes to begin.")
        self._label(left, textvariable=self.ready_var, font=("Segoe UI", 12, "bold")).pack(pady=(14, 6))
        self._label(left, "Hold Space to talk, or type on the right.", font=("Segoe UI", 9)).pack()
        controls = tk.Frame(left, bg=BG)
        controls.pack(pady=12)
        self.talk = self._action(controls, "Hold to talk", primary=True)
        self.talk.bind("<ButtonPress-1>", lambda e: self._button("talk", True))
        self.talk.bind("<ButtonRelease-1>", lambda e: self._button("talk", False))
        self.talk.pack(side="left", padx=5)
        cancel = self._action(controls, "Cancel  ·  Esc")
        cancel.bind("<ButtonPress-1>", lambda e: self._button("cancel", True))
        cancel.bind("<ButtonRelease-1>", lambda e: self._button("cancel", False))
        cancel.pack(side="left", padx=5)
        if b.scroll_buttons:
            self._action(controls, "↑", lambda: self.sim.tap("up")).pack(side="left", padx=3)
            self._action(controls, "↓", lambda: self.sim.tap("down")).pack(side="left", padx=3)
        self._label(left, "Hold Esc for a new conversation · Ctrl+S saves the device screen", font=("Segoe UI", 8)).pack()
        if b.touch:
            z = self.zoom / self._shrink
            self.screen.bind("<ButtonPress-1>", lambda e: self.sim.touch(True, int(e.x / z), int(e.y / z)))
            self.screen.bind("<B1-Motion>", lambda e: self.sim.touch(True, int(e.x / z), int(e.y / z)))
            self.screen.bind("<ButtonRelease-1>", lambda e: self.sim.touch(False))

        chat = tk.Frame(main, bg=PANEL, padx=22, pady=22)
        self.chat = chat
        chat.grid(row=0, column=1, sticky="nsew")
        self._label(chat, "Conversation", font=("Segoe UI", 12, "bold")).pack(anchor="w")
        self._label(chat, "This window only · last 100 messages", font=("Segoe UI", 8)).pack(anchor="w", pady=(4, 12))
        self.conversation = tk.Text(chat, width=40, height=12, wrap="word", bg=PANEL, fg=INK,
                                    relief="flat", padx=3, pady=8, state="disabled", cursor="arrow")
        self.conversation.tag_configure("You", foreground=MUTED, spacing1=16, spacing3=6)
        self.conversation.tag_configure("Hermes", foreground=AMBER, spacing1=16, spacing3=6)
        self.conversation.pack(fill="both", expand=True)
        actions = tk.Frame(chat, bg=PANEL)
        actions.pack(fill="x", pady=8)
        self.copy_reply = self._action(actions, "Copy reply", self._copy_reply)
        self.copy_reply.pack(side="left")
        self.replay_reply = self._action(actions, "Play last audio", self._play_reply)
        self.replay_reply.pack(side="left", padx=8)
        self.audio_var = tk.StringVar()
        self._label(chat, textvariable=self.audio_var, justify="left", anchor="w", wraplength=345,
                    font=("Segoe UI", 9)).pack(fill="x", pady=(12, 8))
        inputs = tk.Frame(chat, bg=PANEL)
        inputs.pack(fill="x")
        self.text_entry = self._entry(inputs)
        self.text_entry.pack(side="left", fill="x", expand=True, ipady=10)
        self.text_entry.bind("<Return>", lambda e: self._send_text())
        self._action(inputs, "Send", self._send_text, primary=True).pack(side="left", padx=(8, 0))
        self._action(chat, "Speak WAV...", self._speak_wav).pack(anchor="e", pady=(10, 0))

        self.developer = tk.Frame(main, bg=PANEL, padx=20, pady=22)
        self.developer.grid(row=0, column=1, sticky="nsew")
        self._build_developer()
        self.developer.grid_remove()
        footer = tk.Frame(self.root, bg=BG, padx=16, pady=8)
        footer.grid(row=3, column=0, sticky="ew")
        self.developer_toggle = self._action(footer, "Developer tools  ▸", self._toggle_developer)
        self.developer_toggle.pack(side="left")
        self.notice_var = tk.StringVar(value="Sensors, actions, serial console, and logs are in Developer tools.")
        self._label(footer, textvariable=self.notice_var, font=("Segoe UI", 9), wraplength=690).pack(side="left", padx=16)

        self.root.bind("<KeyPress-space>", lambda e: self._key("talk", True))
        self.root.bind("<KeyRelease-space>", lambda e: self._key("talk", False))
        self.root.bind("<KeyPress-Escape>", lambda e: self._key("cancel", True, always=True))
        self.root.bind("<KeyRelease-Escape>", lambda e: self._key("cancel", False, always=True))
        self.root.bind("<Up>", lambda e: self._scroll("up"))
        self.root.bind("<Down>", lambda e: self._scroll("down"))
        self.root.bind("<Control-s>", lambda e: self._screenshot())
        self.root.bind("<FocusOut>", lambda e: self.root.after_idle(self._release_if_unfocused))
        self.root.protocol("WM_DELETE_WINDOW", self._quit)

    def _build_developer(self):
        self.developer.columnconfigure(0, weight=1)
        self.developer.rowconfigure(1, weight=1)
        device = tk.Frame(self.developer, bg=PANEL)
        device.grid(row=0, column=0, sticky="nsew", pady=(0, 15))
        self.status_var = tk.StringVar()
        self._label(device, textvariable=self.status_var, justify="left", anchor="w", wraplength=350,
                    font=("Consolas", 9)).pack(fill="x")
        row = tk.Frame(device, bg=PANEL)
        row.pack(fill="x", pady=7)
        self.led = tk.Canvas(row, width=22, height=22, bg=PANEL, highlightthickness=0)
        self.led_dot = self.led.create_oval(3, 3, 19, 19, fill=_LED_COLORS["off"], outline=LINE)
        self.led.pack(side="left")
        self.periph_var = tk.StringVar()
        self._label(row, textvariable=self.periph_var, font=("Segoe UI", 8)).pack(side="left", padx=4)
        self.net_var = tk.BooleanVar(value=self.sim.network_up)
        tk.Checkbutton(row, text="Wi-Fi", variable=self.net_var, selectcolor=BG, activebackground=PANEL,
                       command=lambda: self.sim.set_network(self.net_var.get())).pack(side="right")
        sensors = tk.Frame(device, bg=PANEL)
        sensors.pack(fill="x")
        for key, label, low, high, value in [("battery_pct", "Battery %", 0, 100, self.sim.peripherals.battery),
                                            ("temperature_c", "Temperature °C", -10, 45, self.sim.peripherals.temperature_c)]:
            scale = tk.Scale(sensors, from_=low, to=high, resolution=0.5, orient="horizontal", label=label,
                             bg=PANEL, fg=INK, troughcolor=BG, highlightthickness=0, length=170,
                             command=lambda v, k=key: self.sim.set_sensor(k, float(v)))
            scale.set(value)
            scale.pack(side="left", fill="x", expand=True)
        self._action(device, "Send button.long_press event",
                     lambda: self.sim.device.emit_event("button.long_press", {"button": "aux"}, False)).pack(anchor="w", pady=5)
        logs = tk.Frame(self.developer, bg=PANEL)
        logs.grid(row=1, column=0, sticky="nsew")
        self._label(logs, "Serial console · Enter sends a command", font=("Segoe UI", 9)).pack(anchor="w")
        self.console_entry = self._entry(logs)
        self.console_entry.pack(fill="x", ipady=4, pady=5)
        self.console_entry.insert(0, "help")
        self.console_entry.bind("<Return>", lambda e: self._console())
        self.log = tk.Text(logs, height=7, width=40, bg="#0b1119", fg=MUTED, font=("Consolas", 9), wrap="word", relief="flat", state="disabled")
        self.log.pack(fill="both", expand=True)

    def _toggle_developer(self):
        opening = not self.developer.winfo_ismapped()
        if opening:
            self.chat.grid_remove()
            self.developer.grid()
        else:
            self.developer.grid_remove()
            self.chat.grid()
        self.developer_toggle.configure(text="Back to conversation" if opening else "Developer tools  ▸")

    def _focus_is_entry(self) -> bool:
        return isinstance(self.root.focus_get(), (tk.Entry, tk.Text, ttk.Entry, ttk.Combobox))

    def _button(self, name: str, down: bool) -> None:
        if down == (name in self._held):
            return
        if down:
            self._held.add(name)
            self.sim.press(name)
        else:
            self._held.discard(name)
            self.sim.release(name)

    def _key(self, name: str, down: bool, always: bool = False) -> None:
        if down and not always and self._focus_is_entry():
            return
        if down:
            job = self._release_jobs.pop(name, None)
            if job is not None:
                self.root.after_cancel(job)
            self._button(name, True)
        elif name not in self._release_jobs:
            self._release_jobs[name] = self.root.after(40, lambda: self._key_release(name))

    def _key_release(self, name: str) -> None:
        self._release_jobs.pop(name, None)
        self._button(name, False)

    def _release_controls(self):
        for job in self._release_jobs.values():
            self.root.after_cancel(job)
        self._release_jobs.clear()
        for name in tuple(self._held):
            self._button(name, False)
        if self.sim.board.touch:
            self.sim.touch(False)

    def _release_if_unfocused(self):
        if self.root.focus_get() is None or self._focus_is_entry():
            self._release_controls()

    def _scroll(self, direction: str):
        if self.sim.board.scroll_buttons and not self._focus_is_entry():
            self.sim.tap(direction)

    def _send_text(self):
        text = self.text_entry.get().strip()
        if not text:
            return
        status = self.sim.status()
        if status.get("phase") != "online" or not status.get("paired"):
            self.notice_var.set("Connect and pair the device before sending. Your message is still in the box.")
            return
        self.sim.type_text(text)
        self.text_entry.delete(0, "end")
        self.notice_var.set("Message sent.")

    def _speak_wav(self):
        path = filedialog.askopenfilename(parent=self.root, filetypes=[("WAV audio", "*.wav"), ("All files", "*.*")])
        if path:
            try:
                seconds = self.sim.speak_wav(path)
                self.notice_var.set(f"Sending WAV audio ({seconds:.1f}s).")
            except Exception as exc:
                self.notice_var.set(f"Could not open the recording: {exc}")

    def _console(self):
        line = self.console_entry.get().strip()
        if line:
            self._append(f"$ {line}\n{self.sim.console(line)}")
            self.console_entry.delete(0, "end")

    def _screenshot(self):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        base = self.sim.state_dir or Path(".")
        try:
            path = self.sim.screenshot(base / "screenshots" / f"screen-{stamp}.png")
            self.notice_var.set(f"Saved {path}")
        except OSError as exc:
            self.notice_var.set(f"Could not save the screen: {exc}")

    def _copy_reply(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.history.latest_reply())
        self.notice_var.set("Reply copied.")

    def _play_reply(self):
        path = self.sim.speaker.last_file
        if path is None:
            return
        try:
            with wave.open(str(path), "rb") as audio:
                rate, pcm = audio.getframerate(), audio.readframes(audio.getnframes())
            self._replay.set_live(True)
            self._replay.begin(rate)
            if self._replay.error:
                raise RuntimeError(self._replay.error)
            self._replay.write(pcm)
            self._replay.end()
            self.notice_var.set("Playing the last recorded reply through the system default output.")
        except Exception as exc:
            self._replay.abort()
            self.notice_var.set(f"Audio could not play: {exc}")

    def _message(self, direction: str, message: dict):
        if message.get("type") in ("text", "audio.start", "turn.start", "session.new"):
            self._replay.abort()
        if direction == "sent" and message.get("type") == "session.new":
            self.sim.speaker.last_file = None
        self.history.receive(direction, message)

    def _render_history(self):
        if self._history_revision == self.history.revision:
            return
        self._history_revision = self.history.revision
        self.conversation.configure(state="normal")
        self.conversation.delete("1.0", "end")
        if not self.history.messages:
            self.conversation.insert("end", "Your messages and replies appear here.\n\nConnect a gateway, approve the pairing code, then type or hold TALK.")
        for message in self.history.messages:
            self.conversation.insert("end", message.role + "\n", message.role)
            self.conversation.insert("end", message.text + "\n\n")
        self.conversation.configure(state="disabled")
        self.conversation.see("end")

    def _settings(self):
        if self._settings_window is not None and self._settings_window.winfo_exists():
            self._settings_window.lift()
            return
        dialog = tk.Toplevel(self.root)
        self._settings_window = dialog
        dialog.title("Simulator settings")
        dialog.configure(bg=PANEL, padx=24, pady=20)
        dialog.transient(self.root)
        self._label(dialog, "Connection and audio", font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(0, 15))
        self._label(dialog, "Hermes address").pack(anchor="w")
        url = self._entry(dialog)
        url.configure(width=52)
        url.insert(0, self.sim.status().get("server", ""))
        url.pack(fill="x", ipady=7, pady=(5, 15))
        self._label(dialog, "Board profile · changing it restarts the simulated device").pack(anchor="w")
        board = ttk.Combobox(dialog, values=list(BOARDS), state="readonly")
        board.set(self.sim.board.name)
        board.pack(fill="x", pady=(5, 15))
        mic = tk.BooleanVar(value=self.sim.mic.live)
        speaker = tk.BooleanVar(value=self.sim.speaker.live)
        for label, var in [("Use the system default microphone", mic), ("Play replies on the system default output", speaker)]:
            tk.Checkbutton(dialog, text=label, variable=var, selectcolor=BG, activebackground=PANEL,
                           activeforeground=INK).pack(anchor="w", pady=4)
        self._label(dialog, 'Audio needs: python -m pip install -e ".[audio]"\nChange the input or output device in your system sound settings.',
                    justify="left", font=("Segoe UI", 9)).pack(anchor="w", pady=12)
        self._label(dialog, textvariable=self.notice_var, wraplength=450, justify="left").pack(fill="x", pady=8)

        def save():
            if self._apply_settings(url.get().strip(), board.get(), mic.get(), speaker.get()):
                dialog.destroy()

        buttons = tk.Frame(dialog, bg=PANEL)
        buttons.pack(fill="x", pady=8)
        self._action(buttons, "Save settings", save, primary=True).pack(side="left")
        self._action(buttons, "Reconnect", lambda: self.sim.console("reconnect")).pack(side="left", padx=8)
        self._action(buttons, "Close", dialog.destroy).pack(side="left")
        dialog.bind("<Escape>", lambda e: dialog.destroy())
        url.focus_set()

    def _apply_settings(self, url: str, board: str, mic: bool, speaker: bool) -> bool:
        try:
            address = urlsplit(url)
            if (address.scheme not in ("ws", "wss") or not address.hostname or address.fragment
                    or any(c.isspace() for c in url) or len(url.encode()) > 200):
                raise ValueError("Enter a ws:// or wss:// address from hermes gadget info.")
            _ = address.port
            if self.sim.mic.active:
                raise ValueError("Finish the recording before changing settings.")
            # Check optional dependencies before changing either audio control or the device.
            from .audio_io import sounddevice_available
            if (mic or speaker) and not sounddevice_available():
                raise RuntimeError('Install the audio extra: python -m pip install -e ".[audio]"')
            if board != self.sim.board.name:
                draft = self.text_entry.get()
                replacement = Simulator(url=url, board=board, name=self.sim.status()["name"],
                                        state_dir=self.sim.state_dir, library=self.sim.library)
                self._release_controls()
                self.sim.close()
                self.sim = replacement
                self._replay.abort()
                # Keep the settings dialog until its Save handler closes it.
                for widget in self.root.winfo_children():
                    if not isinstance(widget, tk.Toplevel):
                        widget.destroy()
                self._build()
                self.text_entry.insert(0, draft)
                self._attach()
                self.sim.start()
            elif url != self.sim.status().get("server"):
                result = self.sim.console(f"set server {url}")
                if result.startswith("@error"):
                    raise ValueError(result.removeprefix("@error "))
            self.sim.mic.set_live(mic)
            self.sim.speaker.set_live(speaker)
            self.notice_var.set("Settings saved. Audio changes apply to the next recording or reply.")
            return True
        except (ValueError, RuntimeError, OSError) as exc:
            self.notice_var.set(str(exc))
            return False

    def _on_log(self, level: int, message: str):
        self._append(("DBG ", "INFO", "WARN", "ERR ")[min(level, 3)] + " " + message)

    def _append(self, line: str):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.delete("1.0", "end-500l")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _refresh_peripherals(self):
        p = self.sim.peripherals
        self.led.itemconfigure(self.led_dot, fill=_LED_COLORS.get(p.led, p.led if p.led.startswith("#") else "#888"))
        self.periph_var.set(f"LED · beeps {p.buzzer_count} · light {p.backlight}% · volume {p.volume}%")

    def _redraw(self, y0: int, y1: int):
        b = self.sim.board
        stride = b.width * 3
        self._rgb[y0 * stride:y1 * stride] = self.sim.rgb888(y0, y1)
        if b.round:
            background = bytes.fromhex(BG[1:])
            for y in range(y0, y1):
                x0, x1 = self._round_spans[y]
                self._rgb[y * stride:y * stride + x0 * 3] = background * x0
                self._rgb[y * stride + x1 * 3:(y + 1) * stride] = background * (b.width - x1)
        ppm = png.encode_ppm(bytes(self._rgb), b.width, b.height)
        try:
            image = tk.PhotoImage(data=ppm, format="PPM", master=self.root)
        except tk.TclError:
            image = tk.PhotoImage(data=base64.b64encode(png.encode_png(bytes(self._rgb), b.width, b.height)), master=self.root)
        if self.zoom > 1:
            image = image.zoom(self.zoom)
        if self._shrink > 1:
            image = image.subsample(self._shrink)
        self._photo = image
        self.screen.configure(image=image)

    def _refresh(self):
        st = self.sim.status()
        connected = st.get("phase") == "online"
        if connected and st.get("paired"):
            self.connection_var.set("● Connected · paired")
            self.connection_label.configure(fg="#81cbb2")
        elif st.get("pairing_code"):
            self.connection_var.set(f"Pairing code: {st['pairing_code']}")
        else:
            self.connection_var.set("Offline" if not st.get("network") else "Connecting to gateway")
        if not connected or not st.get("paired"):
            self.connection_label.configure(fg=AMBER)
        self.ready_var.set({"ready":"Ready for your next question.", "listening":"Listening. Release to send.",
                            "thinking":"Waiting for your Hermes...", "responding":"Your Hermes is replying.",
                            "pairing":"Approve the code on the device.", "prompt":"TALK for yes · CANCEL for no.",
                            "error":"Check the device screen for the error."}.get(st.get("screen"), "Connect your Hermes to begin."))
        self.status_var.set(f"{st.get('device_id')}  ·  {st.get('screen')}\n{st.get('server')}")
        self.net_var.set(self.sim.network_up)
        mic = "Microphone on · system default" if self.sim.mic.live else "Microphone off · TALK supplies silence"
        output = "Speaker on" if self.sim.speaker.live else "Speaker off · replies saved as WAV"
        if not self.sim.board.speaker:
            output = "No speaker on this board · text replies"
        if self.sim.mic.error:
            mic = "Microphone unavailable: " + self.sim.mic.error
        if self.sim.speaker.error:
            output = "Speaker unavailable: " + self.sim.speaker.error
        self.audio_var.set(mic + "\n" + output)
        self.copy_reply.configure(state="normal" if self.history.latest_reply() else "disabled")
        self.replay_reply.configure(state="normal" if self.sim.speaker.last_file else "disabled")
        self._render_history()
        self._replay.busy()

    def _tick(self):
        self.sim.step()
        rows = self.sim.take_dirty_rows()
        if rows:
            self._redraw(*rows)
        self._refresh()
        if time.monotonic() - self._last_sensor > 30:
            self._last_sensor = time.monotonic()
            self.sim.drift_sensors()
        self.root.after(15, self._tick)

    def _quit(self):
        self._release_controls()
        self._replay.abort()
        self.sim.close()
        self.root.destroy()

    def run(self):
        self.sim.start(network=True)
        self._refresh_peripherals()
        self._append("Simulator started. " + KEYS_HELP)
        self.root.after(15, self._tick)
        self.root.mainloop()
