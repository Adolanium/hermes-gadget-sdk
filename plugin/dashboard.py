"""The gadget dashboard: Hermes's sessions, the user's status tiles and home-screen widgets.

Session state comes from Hermes's plugin hooks, which run on agent threads:
``pre_llm_call`` starts a turn (busy), ``on_session_end`` ends it (idle), the
approval hooks bracket a wait for the user (waiting), and ``on_session_finalize``
or ``on_session_reset`` retire a session. ``SessionBoard`` keeps that state
behind a lock and calls a listener on every change; the adapter turns it into a
``dashboard`` message (docs/protocol.md#dashboard).

Tiles are lines the user sets from the host, ``hermes gadget tile set nas up``,
for homelab services and the like. They live in a JSON file in the plugin's data
directory, so a script or cron job can update them from another process.

Widgets are full-screen pages a device shows from its home screen: a big value, a
progress bar, a short list or a two-line headline. They live in the same file, set
with ``hermes gadget widget`` or the ``gadget_widget`` tool, for every device or
for one. A device that draws only layout 1 gets them as tiles on its list.
"""

from __future__ import annotations

import contextlib
import json
import math
import os
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable

from . import runtime, textfmt

VERSION = 2                # the newest dashboard message layout this plugin sends (2 adds widgets)
MAX_SESSIONS = 8
MAX_TILES = 8
MAX_WIDGETS = 8            # pages one device gets
MAX_ITEMS = 4              # lines of a list widget
MAX_TEXT = 200             # characters kept of any widget text; the message cuts it to the device's line
WIDGET_KINDS = ("value", "progress", "list", "headline")
ALL_DEVICES = "*"          # in the file: the widgets every device gets
RETRIES, RETRY_S = 10, 0.01  # a file Windows holds for another process frees up within milliseconds
LOCK_WAIT_S = 10.0         # how long a change waits for another one to finish
IDLE_KEEP_S = 30 * 60      # an idle session leaves the dashboard this long after its last turn
ACTIVE_KEEP_S = 6 * 3600   # ...and one that never reported its end, after this long
TILE_STATES = ("ok", "warn", "bad")
_ORDER = {"waiting": 0, "busy": 1, "idle": 2}


class SessionBoard:
    """Live state of the agent sessions in this process, fed by Hermes's hooks."""

    def __init__(self, clock: Callable[[], float] = time.monotonic):
        self._clock = clock
        self._lock = threading.Lock()
        self._sessions: dict[str, dict[str, Any]] = {}
        self._listeners: list[Callable[[], None]] = []

    def subscribe(self, listener: Callable[[], None]) -> Callable[[], None]:
        """Call ``listener`` (on the hook's thread) after each change. Returns the unsubscribe."""
        with self._lock:
            self._listeners.append(listener)

        def unsubscribe() -> None:
            with self._lock:
                if listener in self._listeners:
                    self._listeners.remove(listener)

        return unsubscribe

    def _changed(self) -> None:
        with self._lock:
            listeners = list(self._listeners)
        for listener in listeners:
            try:
                listener()
            except Exception:  # a listener's trouble is never the agent's
                pass

    def _set(self, session_id: Any, platform: Any = None, *, approvals: int = 0, state: str | None = None) -> None:
        """Record a session's state; ``approvals`` counts open approvals (one wait can overlap another)."""
        sid = str(session_id or "")
        if not sid:
            return
        with self._lock:
            new = sid not in self._sessions
            rec = self._sessions.setdefault(sid, {"platform": "", "state": "idle", "approvals": 0})
            if platform:
                rec["platform"] = str(platform)
            before = (rec["state"], rec["approvals"])
            if approvals:
                rec["approvals"] = max(0, rec["approvals"] + approvals)
                rec["state"] = "waiting" if rec["approvals"] else "busy"
            if state:  # a turn starting or ending settles every approval
                rec["state"], rec["approvals"] = state, 0
            rec["at"] = self._clock()
            changed = new or before != (rec["state"], rec["approvals"])
        if changed:
            self._changed()

    def _drop(self, *session_ids: Any) -> None:
        with self._lock:
            gone = [self._sessions.pop(str(s), None) for s in session_ids if s]
        if any(gone):
            self._changed()

    # -- Hermes hooks (keyword arguments only; Hermes adds fields over time) ----------

    def on_session_start(self, session_id=None, platform=None, **_kw) -> None:
        self._set(session_id, platform)

    def pre_llm_call(self, session_id=None, platform=None, parent_session_id=None, **_kw) -> None:
        if parent_session_id:
            return None  # a subagent works for a session that is already shown busy
        self._set(session_id, platform, state="busy")
        return None  # adds no context to the turn

    def on_session_end(self, session_id=None, platform=None, **_kw) -> None:
        self._set(session_id, platform, state="idle")

    def pre_approval_request(self, session_id=None, **_kw) -> None:
        self._set(session_id, approvals=+1)

    def post_approval_response(self, session_id=None, **_kw) -> None:
        self._set(session_id, approvals=-1)

    def on_session_finalize(self, session_id=None, **_kw) -> None:
        self._drop(session_id)

    def on_session_reset(self, session_id=None, old_session_id=None, **_kw) -> None:
        self._drop(old_session_id)

    HOOKS = ("on_session_start", "pre_llm_call", "on_session_end", "pre_approval_request",
             "post_approval_response", "on_session_finalize", "on_session_reset")

    def register(self, ctx) -> None:
        for name in self.HOOKS:
            ctx.register_hook(name, getattr(self, name))
        # Hermes loads a copy of the plugin per profile, and a turn fires only its own profile's
        # hooks. A device switched to this profile reads this board too (adapter._push_dashboard).
        runtime.add_board(getattr(ctx, "profile_name", None) or "default", self)

    # -- reading ------------------------------------------------------------------------

    def _live(self) -> list[tuple[str, dict[str, Any]]]:
        now = self._clock()
        with self._lock:
            for sid in [s for s, rec in self._sessions.items()
                        if now - rec.get("at", now) > (IDLE_KEEP_S if rec["state"] == "idle" else ACTIVE_KEEP_S)]:
                del self._sessions[sid]
            return [(sid, dict(rec)) for sid, rec in self._sessions.items()]

    def sessions(self, *others: SessionBoard) -> list[dict[str, str]]:
        """Sessions worth showing, with those of ``others``: waiting first, then busy, then idle;
        newest first within each."""
        rows = sorted((row for board in (self, *others) for row in board._live()),
                      key=lambda kv: (_ORDER[kv[1]["state"]], -kv[1].get("at", 0)))
        return [{"id": sid[-4:], "label": f"{rec['platform'] or 'session'} {sid[-4:]}", "state": rec["state"]}
                for sid, rec in rows[:MAX_SESSIONS]]


BOARD = SessionBoard()  # this process's sessions; the plugin registers its hooks


def _os_lock(fh) -> None:
    """Take the operating system's exclusive lock on an open file, or raise OSError when another
    open file holds it. Closing the file, or its process ending, releases it."""
    if os.name == "nt":
        import msvcrt

        fh.seek(0)
        msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl

        fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)


class TileStore:
    """Status tiles and widgets in ``dashboard.json``; written by the CLI and the agent's tool,
    read by the gateway. Every change is one locked read-modify-write of the whole file."""

    FILENAME = "dashboard.json"

    def __init__(self, directory: Path | str, clock: Callable[[], float] = time.time):
        self.path = Path(directory) / self.FILENAME
        self._clock = clock

    @contextlib.contextmanager
    def _locked(self):
        """Hold the file for one read-modify-write. The CLI, a cron job, the agent's tool and the
        gateway each change it from their own process or thread; without this, two changes read
        the same file and the later write drops the earlier one."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        lock = self.path.with_name(self.path.name + ".lock")
        deadline = time.monotonic() + LOCK_WAIT_S
        # The lock is the operating system's, on the open file: it ends with its holder, so a
        # process that died holds nothing and no lock is ever taken over by its age.
        while True:
            fh = None
            try:
                fh = open(lock, "a+b")  # Windows refuses this while the file is being removed
                _os_lock(fh)
                # The holder before removed the file as it let go: this one is no longer the
                # file the next writer opens. Start over on the one that is.
                if os.name == "nt" or os.fstat(fh.fileno()).st_ino == os.stat(lock).st_ino:
                    break
            except OSError:
                pass
            if fh is not None:
                fh.close()
            if time.monotonic() >= deadline:
                raise TimeoutError(f"{self.path.name} is locked by another process") from None
            time.sleep(0.002)
        try:
            yield
        finally:
            # Removed so the folder holds the dashboard alone. POSIX: while the lock is held, so a
            # waiter sees the file is gone. Windows refuses to remove a file that is open: after
            # closing, and not at all while another writer has it open.
            if os.name != "nt":
                with contextlib.suppress(OSError):
                    lock.unlink()
            fh.close()
            if os.name == "nt":
                with contextlib.suppress(OSError):
                    lock.unlink()

    def _load(self) -> dict[str, dict]:
        """The file's two sections: ``tiles`` (name -> tile) and ``widgets`` (device id or ``*`` ->
        name -> widget). A missing file is an empty one, and anything malformed in it reads as
        absent. A file that cannot be read is neither: the error is raised, so that a busy file
        is never written back as an empty one."""
        raw = None
        for attempt in range(RETRIES):
            try:
                raw = json.loads(self.path.read_text(encoding="utf-8"))
                break
            except FileNotFoundError:
                break
            except ValueError:
                break
            except OSError:
                # Windows refuses a read while another process replaces the file; it frees up
                # within milliseconds, so try a few times before giving up.
                if attempt == RETRIES - 1:
                    raise
                time.sleep(RETRY_S)
        raw = raw if isinstance(raw, dict) else {}

        def table(value: Any) -> dict[str, dict]:
            return {str(k): v for k, v in value.items() if isinstance(v, dict)} if isinstance(value, dict) else {}

        return {"tiles": table(raw.get("tiles")),
                "widgets": {scope: table(rows) for scope, rows in table(raw.get("widgets")).items()}}

    def _save(self, doc: dict[str, dict]) -> None:
        out = {"tiles": doc["tiles"]}
        if any(doc["widgets"].values()):
            out["widgets"] = {scope: rows for scope, rows in doc["widgets"].items() if rows}
        text = json.dumps(out, indent=2, allow_nan=False)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # A unique temporary file (as store.py does): two writers never share one.
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=f"{self.path.stem}.", suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(text)
            for attempt in range(RETRIES):
                try:
                    os.replace(tmp, self.path)
                    break
                except PermissionError:  # Windows: a reader holds the file at this moment
                    if attempt == RETRIES - 1:
                        raise
                    time.sleep(RETRY_S)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise

    def _expiry(self, ttl_s: Any) -> float | None:
        """When something set now with ``ttl_s`` expires; None for no expiry."""
        if ttl_s is None:
            return None
        try:
            seconds = float(ttl_s)
        except (TypeError, ValueError):
            seconds = float("nan")
        if isinstance(ttl_s, bool) or not math.isfinite(seconds) or seconds <= 0:
            raise ValueError("ttl must be a positive number of seconds")
        return self._clock() + seconds

    def mtime(self) -> float:
        try:
            return self.path.stat().st_mtime
        except OSError:
            return 0.0

    def set(self, name: str, value: str, *, state: str | None = None, label: str | None = None,
            ttl_s: float | None = None) -> dict:
        name = name.strip()
        if not name:
            raise ValueError("a tile needs a name")
        if state is not None and state not in TILE_STATES:
            raise ValueError(f"state must be one of {', '.join(TILE_STATES)}")
        tile = {"label": label or name, "value": value}
        if state:
            tile["state"] = state
        expires = self._expiry(ttl_s)
        if expires is not None:
            tile["expires"] = expires
        with self._locked():
            doc = self._load()
            live = self._live_tiles(doc["tiles"])
            # An expired tile set again is a new one: its name is still stored, and it is not shown.
            if name not in {t["id"] for t in live} and len(live) >= MAX_TILES:
                raise ValueError(f"the dashboard holds {MAX_TILES} tiles; clear one first")
            doc["tiles"][name] = tile
            self._save(doc)
        return tile

    def clear(self, name: str | None = None) -> bool:
        with self._locked():
            doc = self._load()
            if not (doc["tiles"] if name is None else name in doc["tiles"]):
                return False
            if name is None:
                doc["tiles"] = {}
            else:
                del doc["tiles"][name]
            self._save(doc)
        return True

    def _live_tiles(self, tiles: dict[str, dict]) -> list[dict[str, str]]:
        now = self._clock()
        out = []
        for name, tile in tiles.items():
            expires = tile.get("expires")
            if isinstance(expires, (int, float)) and expires <= now:
                continue
            row = {"id": name, "label": str(tile.get("label") or name), "value": str(tile.get("value") or "")}
            if tile.get("state") in TILE_STATES:
                row["state"] = tile["state"]
            out.append(row)
        return out

    def tiles(self) -> list[dict[str, str]]:
        """Tiles that haven't expired, in the order they were first set."""
        return self._live_tiles(self._load()["tiles"])[:MAX_TILES]

    # -- widgets ------------------------------------------------------------------------

    def set_widget(self, name: str, kind: str, *, device: str | None = None, label: str | None = None,
                   value: Any = None, unit: str | None = None, percent: Any = None, items: Any = None,
                   title: str | None = None, text: str | None = None, state: str | None = None,
                   ttl_s: float | None = None) -> dict:
        """Add a widget or replace the one of that name, for ``device`` (a device id) or for every
        device. Raises ValueError for what no device could draw, and for a widget that would leave
        a device with more than it shows: nothing already on a screen is ever pushed off it."""
        name = str(name or "").strip()
        if not name:
            raise ValueError("a widget needs a name")
        if kind not in WIDGET_KINDS:
            raise ValueError(f"kind must be one of {', '.join(WIDGET_KINDS)}")
        if state is not None and state not in TILE_STATES:
            raise ValueError(f"state must be one of {', '.join(TILE_STATES)}")

        def line(v: Any) -> str:
            return " ".join(str(v if v is not None else "").split())[:MAX_TEXT]

        widget: dict[str, Any] = {"kind": kind, "label": line(label) or name}
        if kind == "value":
            if not line(value):
                raise ValueError("a value widget needs a value")
            widget["value"] = line(value)
            if line(unit):
                widget["unit"] = line(unit)
        elif kind == "progress":
            try:
                number = float(percent)
            except (TypeError, ValueError):
                number = float("nan")
            if isinstance(percent, bool) or not math.isfinite(number):
                raise ValueError("a progress widget needs a percent from 0 to 100")
            widget["percent"] = int(round(max(0.0, min(100.0, number))))
        elif kind == "list":
            rows = [line(i) for i in items] if isinstance(items, (list, tuple)) else []
            rows = [r for r in rows if r]
            if not rows:
                raise ValueError("a list widget needs at least one item")
            if len(rows) > MAX_ITEMS:
                raise ValueError(f"a list widget holds {MAX_ITEMS} items")
            widget["items"] = rows
        else:
            if not line(title):
                raise ValueError("a headline widget needs a title")
            widget["title"] = line(title)
            if line(text):
                widget["text"] = line(text)
        if state:
            widget["state"] = state
        expires = self._expiry(ttl_s)
        if expires is not None:
            widget["expires"] = expires
        scope = device or ALL_DEVICES
        with self._locked():
            doc = self._load()
            # What each device it reaches would show with it; a name a device already shows takes
            # no new place. A shared widget reaches every device, those with their own included.
            reached = [device] if device else [None, *(d for d in doc["widgets"] if d != ALL_DEVICES)]
            for d in reached:
                if len(set(self._merged(doc, d)) | {name}) > MAX_WIDGETS:
                    who = "a gadget" if d is None or d == device else f"the gadget {d}, with its own,"
                    raise ValueError(f"{who} shows {MAX_WIDGETS} widgets; clear one first")
            doc["widgets"].setdefault(scope, {})[name] = widget
            self._save(doc)
        return widget

    def clear_widgets(self, name: str | None = None, *, device: str | None = None) -> bool:
        """Remove the widget ``name``, or every widget, set for ``device`` or for every device.
        False when there was nothing to remove."""
        with self._locked():
            doc = self._load()
            rows = doc["widgets"].get(device or ALL_DEVICES, {})
            if not (rows if name is None else name in rows):
                return False
            if name is None:
                rows.clear()
            else:
                del rows[name]
            self._save(doc)
        return True

    def _live(self, rows: dict[str, dict]) -> dict[str, dict]:
        now = self._clock()
        return {name: w for name, w in rows.items()
                if w.get("kind") in WIDGET_KINDS
                and not (isinstance(w.get("expires"), (int, float)) and w["expires"] <= now)}

    def _merged(self, doc: dict, device: str | None) -> dict[str, dict]:
        merged = dict(self._live(doc["widgets"].get(ALL_DEVICES, {})))
        if device:
            merged.update(self._live(doc["widgets"].get(device, {})))  # a device's own replaces the shared one
        return merged

    def widgets(self, device: str | None = None) -> list[dict]:
        """The widgets ``device`` shows, in the order they were first set: those for every device,
        each replaced by the device's own of the same name, then the rest of its own."""
        return [_row(name, w) for name, w in self._merged(self._load(), device).items()][:MAX_WIDGETS]

    def widget_scopes(self) -> dict[str, list[dict]]:
        """Every live widget as stored: ``*`` and each device id to the widgets set for it."""
        scopes = {scope: [_row(name, w) for name, w in self._live(rows).items()]
                  for scope, rows in self._load()["widgets"].items()}
        return {scope: rows for scope, rows in scopes.items() if rows}


def _row(name: str, widget: dict) -> dict:
    """A stored widget as the message carries it: its fields as text, numbers and lists a device
    can take, whatever a hand-edited file holds."""
    row: dict[str, Any] = {"id": name, "kind": widget["kind"], "label": str(widget.get("label") or name)}
    for key in ("value", "unit", "title", "text"):
        if widget.get(key):
            row[key] = str(widget[key])
    if widget["kind"] == "progress":
        percent = widget.get("percent")
        ok = isinstance(percent, (int, float)) and not isinstance(percent, bool) and math.isfinite(percent)
        row["percent"] = int(max(0, min(100, percent))) if ok else 0
    if widget["kind"] == "list":
        items = widget.get("items")
        row["items"] = [str(i) for i in items][:MAX_ITEMS] if isinstance(items, list) else []
    if widget.get("state") in TILE_STATES:
        row["state"] = widget["state"]
    return row


def widget_summary(widget: dict) -> str:
    """A widget as one short line: what a list row, a layout 1 tile or a tool result says of it."""
    kind = widget.get("kind")
    if kind == "value":
        return " ".join(p for p in (str(widget.get("value") or ""), str(widget.get("unit") or "")) if p)
    if kind == "progress":
        return f"{widget.get('percent', 0)}%"
    if kind == "list":
        return str((widget.get("items") or [""])[0])
    return str(widget.get("title") or "")


def message_for(session, sessions: list[dict], store: TileStore) -> dict:
    """The ``dashboard`` message for one connected device (a hub ``DeviceSession``): the tiles,
    the widgets set for every device and for this one, in the layout and the text it draws."""
    return message(sessions, store.tiles(), store.widgets(session.device_id), charset=session.charset,
                   cols=session.display.get("text_cols"), version=session.dashboard_version)


def message(sessions: list[dict], tiles: list[dict], widgets: list[dict] | None = None, *,
            charset: str = "ascii", cols: int | None = None, version: int = VERSION) -> dict:
    """The ``dashboard`` message for one device: text folded to its charset and cut to its line.

    ``version`` is the layout the device draws. Layout 2 carries ``widgets``. Layout 1 has no
    place for them, so each becomes a tile on the list while there is room for tiles."""
    width = cols if isinstance(cols, int) and cols > 4 else 40
    version = max(1, min(VERSION, version))

    def shape(text: Any) -> str:
        return textfmt.for_device(str(text), charset).replace("\n", " ")[:width]

    out_tiles = [{**t, "label": shape(t["label"]), "value": shape(t["value"])} for t in tiles]
    msg = {"type": "dashboard", "v": version,
           "sessions": [{**s, "label": shape(s["label"])} for s in sessions], "tiles": out_tiles}
    widgets = (widgets or [])[:MAX_WIDGETS]
    if version < 2:
        for w in widgets[:max(0, MAX_TILES - len(out_tiles))]:
            tile = {"id": w["id"], "label": shape(w.get("label") or w["id"]), "value": shape(widget_summary(w))}
            if w.get("state") in TILE_STATES:
                tile["state"] = w["state"]
            out_tiles.append(tile)
        return msg
    shaped = []
    for w in widgets:
        row = dict(w)
        for key in ("label", "value", "unit", "title", "text"):
            if key in row:
                row[key] = shape(row[key])
        if "items" in row:
            row["items"] = [shape(i) for i in row["items"][:MAX_ITEMS]]
        shaped.append(row)
    msg["widgets"] = shaped
    return msg
