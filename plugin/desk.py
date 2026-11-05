"""Requests from the Hermes Desktop page to a connected device, through the plugin's data folder.

The Desktop page's backend (``dashboard/plugin_api.py``) runs in another process from the gateway,
which holds the device connections. It writes a request file to ``desk/inbox/``; the gateway's
watcher sends it to the device and writes the outcome to ``desk/done/``, which the backend reads
back. The same file-based handoff the firmware update queue and the dashboard tiles use.

Requests are small JSON objects, bounded in size and number, and expire if no gateway picks them
up: a page never waits on a gateway that is not running.
"""

from __future__ import annotations

import json
import os
import re
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

KINDS = ("card", "action")
MAX_PENDING = 32
EXPIRE_S = 30.0  # a request no gateway picked up in this long is dropped
RESULT_KEEP_S = 120.0
MAX_TITLE = 64
MAX_TEXT = 600
_ID = re.compile(r"^[0-9a-f]{32}$")
_DEVICE = re.compile(r"^hg-[0-9a-f]{16}$")
_ACTION = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def _write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".req.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def _read_json(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


class Desk:
    def __init__(self, directory: Path | str):
        self.root = Path(directory) / "desk"
        self.inbox = self.root / "inbox"
        self.done = self.root / "done"

    # -- the page's side -----------------------------------------------------------

    def post(self, device_id: str, kind: str, **fields: Any) -> str:
        """Queue a request and return its id. Raises ValueError with a sentence on bad input."""
        if not _DEVICE.match(device_id or ""):
            raise ValueError("unknown device")
        if kind not in KINDS:
            raise ValueError("unknown request")
        req: dict[str, Any] = {"device": device_id, "kind": kind, "at": time.time()}
        if kind == "card":
            title = str(fields.get("title") or "").strip()[:MAX_TITLE]
            text = str(fields.get("text") or "").strip()[:MAX_TEXT]
            if not title and not text:
                raise ValueError("write something to show")
            try:
                seconds = max(0, min(int(fields.get("seconds", 20)), 3600))
            except (TypeError, ValueError):
                seconds = 20
            req.update(title=title, text=text, seconds=seconds)
        else:
            action = str(fields.get("action") or "")
            if not _ACTION.match(action):
                raise ValueError("unknown action")
            args = fields.get("args") or {}
            if not isinstance(args, dict) or len(json.dumps(args)) > 2000:
                raise ValueError("action arguments must be a small object")
            req.update(action=action, args=args)
        self.inbox.mkdir(parents=True, exist_ok=True)
        if len(list(self.inbox.glob("*.json"))) >= MAX_PENDING:
            raise ValueError("too many requests are waiting; is the gateway running?")
        req_id = uuid.uuid4().hex
        _write_json(self.inbox / f"{req_id}.json", req)
        return req_id

    def result(self, req_id: str) -> dict | None:
        if not _ID.match(req_id or ""):
            return None
        return _read_json(self.done / f"{req_id}.json")

    def wait(self, req_id: str, timeout: float, *, poll: float = 0.15) -> dict:
        """The request's outcome, or ``{"ok": False, "error": ...}`` when no gateway answered in time."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.result(req_id)
            if found is not None:
                return found
            time.sleep(poll)
        # Withdraw it, so a gateway that starts later doesn't act on something the page gave up on.
        (self.inbox / f"{req_id}.json").unlink(missing_ok=True)
        return {"ok": False, "error": "The gateway didn't answer. Is it running with the Gadget platform on?"}

    # -- the gateway's side ----------------------------------------------------------

    def pending(self, now: float | None = None) -> list[tuple[str, dict]]:
        """Requests waiting, oldest first. Expired and malformed ones are removed on the way."""
        now = time.time() if now is None else now
        out = []
        for path in sorted(self.inbox.glob("*.json"), key=lambda p: p.stat().st_mtime if p.exists() else 0):
            req = _read_json(path)
            stale = req is None or not isinstance(req.get("at"), (int, float)) or now - req["at"] > EXPIRE_S
            if stale or not _ID.match(path.stem) or req.get("kind") not in KINDS:
                path.unlink(missing_ok=True)
                continue
            out.append((path.stem, req))
        return out

    def waiting(self, req_id: str) -> bool:
        """Whether the request is still queued: the page withdraws one it stopped waiting for."""
        return bool(_ID.match(req_id or "")) and (self.inbox / f"{req_id}.json").exists()

    def finish(self, req_id: str, outcome: dict, now: float | None = None) -> None:
        now = time.time() if now is None else now
        _write_json(self.done / f"{req_id}.json", {**outcome, "at": now})
        (self.inbox / f"{req_id}.json").unlink(missing_ok=True)
        for old in self.done.glob("*.json"):
            try:
                if now - old.stat().st_mtime > RESULT_KEEP_S:
                    old.unlink(missing_ok=True)
            except OSError:
                pass
