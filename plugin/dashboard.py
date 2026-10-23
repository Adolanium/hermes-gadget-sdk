"""The gadget dashboard: Hermes's sessions and the user's status tiles.

Session state comes from Hermes's plugin hooks, which run on agent threads:
``pre_llm_call`` starts a turn (busy), ``on_session_end`` ends it (idle), the
approval hooks bracket a wait for the user (waiting), and ``on_session_finalize``
or ``on_session_reset`` retire a session. ``SessionBoard`` keeps that state
behind a lock and calls a listener on every change; the adapter turns it into a
``dashboard`` message (docs/protocol.md#dashboard).

Tiles are lines the user sets from the host, ``hermes gadget tile set nas up``,
for homelab services and the like. They live in a JSON file in the plugin's data
directory, so a script or cron job can update them from another process.
"""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Callable

from . import runtime, textfmt

VERSION = 1                # the dashboard message layout this plugin sends
MAX_SESSIONS = 8
MAX_TILES = 8
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


class TileStore:
    """Status tiles in ``dashboard.json``; written by the CLI, read by the gateway."""

    FILENAME = "dashboard.json"

    def __init__(self, directory: Path | str, clock: Callable[[], float] = time.time):
        self.path = Path(directory) / self.FILENAME
        self._clock = clock

    def _read(self) -> dict[str, dict]:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        tiles = raw.get("tiles") if isinstance(raw, dict) else None
        return {str(k): v for k, v in tiles.items() if isinstance(v, dict)} if isinstance(tiles, dict) else {}

    def _write(self, tiles: dict[str, dict]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_name(f"{self.path.name}.{os.getpid()}.tmp")
        tmp.write_text(json.dumps({"tiles": tiles}, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)

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
        tiles = self._read()
        if name not in tiles and len(self.tiles()) >= MAX_TILES:
            raise ValueError(f"the dashboard holds {MAX_TILES} tiles; clear one first")
        tile = {"label": label or name, "value": value}
        if state:
            tile["state"] = state
        if ttl_s:
            tile["expires"] = self._clock() + float(ttl_s)
        tiles[name] = tile
        self._write(tiles)
        return tile

    def clear(self, name: str | None = None) -> bool:
        tiles = self._read()
        if name is None:
            if not tiles:
                return False
            self._write({})
            return True
        if tiles.pop(name, None) is None:
            return False
        self._write(tiles)
        return True

    def tiles(self) -> list[dict[str, str]]:
        """Tiles that haven't expired, in the order they were first set."""
        now = self._clock()
        out = []
        for name, tile in self._read().items():
            expires = tile.get("expires")
            if isinstance(expires, (int, float)) and expires <= now:
                continue
            row = {"id": name, "label": str(tile.get("label") or name), "value": str(tile.get("value") or "")}
            if tile.get("state") in TILE_STATES:
                row["state"] = tile["state"]
            out.append(row)
        return out[:MAX_TILES]


def message(sessions: list[dict], tiles: list[dict], *, charset: str = "ascii", cols: int | None = None) -> dict:
    """The ``dashboard`` message for one device: text folded to its charset and cut to its line."""
    width = cols if isinstance(cols, int) and cols > 4 else 40

    def shape(text: str) -> str:
        return textfmt.for_device(text, charset).replace("\n", " ")[:width]

    return {
        "type": "dashboard",
        "v": VERSION,
        "sessions": [{**s, "label": shape(s["label"])} for s in sessions],
        "tiles": [{**t, "label": shape(t["label"]), "value": shape(t["value"])} for t in tiles],
    }
