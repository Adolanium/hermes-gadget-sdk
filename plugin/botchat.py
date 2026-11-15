"""``hermes gadget bot-chat``: put a gadget in a profile's Bot Chat without a USB console.

Hermes Bot Mode gives the agent of a profile's canonical "Bot Chat" session the ``message_agent``
tool, so that profile can message the other Bots and hear their answers. A gadget's conversation
is its own session; ``/resume --all Bot Chat`` sent from the gadget moves that canonical session
onto the gadget's chat, and Hermes allows it only for an admin of the gadget platform
(``platforms.gadget.extra.allow_admin_from``).

The CLI and the gateway are separate processes and meet in files under the plugin's data
directory, in ``bot-chat/``: ``<device>.json`` (the request) and ``<device>.status.json`` (what the
gateway did with it). The CLI checks the result in the profile's session store itself.
"""

from __future__ import annotations

import json
import os
import sqlite3
import tempfile
import time
import uuid
from pathlib import Path
from typing import Optional

from . import protocol

BOT_CHAT_TITLE = "Bot Chat"
RESUME_COMMAND = f"/resume --all {BOT_CHAT_TITLE}"
REQUEST_TTL_S = 600  # a request for a device that never comes online is dropped after this
VIRTUAL_HINT = ("On Hermes without the validate_native_profile_route hook (up to v0.23.1), also set "
                "profile_mode: virtual, or messages to that profile fail with profile_mismatch.")


class BotChatQueue:
    """One pending request per device, with an id so two runs of the command never take each other's
    request or report: the gateway drops a request only if the file still holds the id it loaded, and
    writes that id into the status the CLI waits for."""

    def __init__(self, directory: Path | str):
        self.dir = Path(directory) / "bot-chat"

    def _path(self, device_id: str, suffix: str) -> Path:
        """A file of the device's, refusing anything that isn't a device id (no path tricks)."""
        if not protocol.DEVICE_ID_RE.match(device_id):
            raise ValueError(f"not a device id: {device_id!r}")
        return self.dir / f"{device_id}{suffix}"

    def _write(self, path: Path, data: dict) -> None:
        """Replace ``path`` atomically, through a temporary file of this write's own (two runs of the
        command may write the same device's request at once)."""
        self.dir.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.dir, prefix=f".{path.name}.", suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(json.dumps(data))
            os.replace(tmp, path)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise

    def request(self, device_id: str, profile: str) -> str:
        """Queue a request (replacing any other for the device) and return its id."""
        request_id = uuid.uuid4().hex
        self._write(self._path(device_id, ".json"),
                    {"id": request_id, "profile": profile, "requested_at": time.time()})
        return request_id

    def pending(self) -> list[str]:
        """The devices with a request waiting."""
        if not self.dir.is_dir():
            return []
        return sorted(p.stem for p in self.dir.glob("*.json")
                      if not p.name.endswith(".status.json") and protocol.DEVICE_ID_RE.match(p.stem))

    def load(self, device_id: str) -> Optional[dict]:
        """The device's request, or None when there is none or it isn't a valid request. Any other read
        error raises: the request may be fine, so it stays for a later pass."""
        try:
            data = json.loads(self._path(device_id, ".json").read_text(encoding="utf-8"))
        except (FileNotFoundError, ValueError):
            return None
        return data if isinstance(data, dict) and isinstance(data.get("profile"), str) else None

    def drop(self, device_id: str, request_id: Optional[str] = None) -> bool:
        """Remove the device's request; with ``request_id``, only if the file holds that one (a newer
        run may have replaced it). True when it was removed.

        The check and the removal are one step: the file is first renamed to a name of this call's own,
        so a request written meanwhile lands on the usual name and is never the one removed. A file that
        turns out to hold another request goes back, unless a newer one has taken its place since."""
        path = self._path(device_id, ".json")
        if request_id is None:
            path.unlink(missing_ok=True)
            return True
        taken = path.with_name(f".{path.name}.{uuid.uuid4().hex}.taken")
        try:
            os.replace(path, taken)
        except FileNotFoundError:
            return False
        try:
            data = json.loads(taken.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data = None
        if isinstance(data, dict) and data.get("id") == request_id:
            taken.unlink(missing_ok=True)
            return True
        try:
            os.link(taken, path)  # put it back; fails if a newer request already took the name
        except FileExistsError:
            pass
        taken.unlink(missing_ok=True)
        return False

    def report(self, device_id: str, **status) -> None:
        """Write what the gateway did with a request, its ``id`` among the fields."""
        self._write(self._path(device_id, ".status.json"), {**status, "at": time.time()})

    def status(self, device_id: str) -> Optional[dict]:
        """The last report for the device, or None."""
        try:
            data = json.loads(self._path(device_id, ".status.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if isinstance(data, dict) else None


def bot_chat_row(state_db: Path) -> Optional[dict]:
    """The profile's canonical Bot Chat session (id and the chat it is bound to), read-only; None when
    the profile has none."""
    if not state_db.exists():
        return None
    try:
        conn = sqlite3.connect(f"file:{state_db}?mode=ro", uri=True, timeout=5)
        try:
            row = conn.execute(
                "SELECT id, source, chat_id FROM sessions WHERE title = ? ORDER BY started_at DESC LIMIT 1",
                (BOT_CHAT_TITLE,)).fetchone()
        finally:
            conn.close()
    except sqlite3.Error:
        return None
    return {"id": row[0], "source": row[1], "chat_id": row[2]} if row else None
