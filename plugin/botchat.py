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
import time
from pathlib import Path
from typing import Optional

from . import protocol

BOT_CHAT_TITLE = "Bot Chat"
RESUME_COMMAND = f"/resume --all {BOT_CHAT_TITLE}"
REQUEST_TTL_S = 600  # a request for a device that never comes online is dropped after this
VIRTUAL_HINT = ("On Hermes without the validate_native_profile_route hook (up to v0.23.1), also set "
                "profile_mode: virtual, or messages to that profile fail with profile_mismatch.")


class BotChatQueue:
    def __init__(self, directory: Path | str):
        self.dir = Path(directory) / "bot-chat"

    def _path(self, device_id: str, suffix: str) -> Path:
        if not protocol.DEVICE_ID_RE.match(device_id):
            raise ValueError(f"not a device id: {device_id!r}")
        return self.dir / f"{device_id}{suffix}"

    def _write(self, path: Path, data: dict) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(json.dumps(data), encoding="utf-8")
        os.replace(tmp, path)

    def request(self, device_id: str, profile: str) -> None:
        self._path(device_id, ".status.json").unlink(missing_ok=True)
        self._write(self._path(device_id, ".json"), {"profile": profile, "requested_at": time.time()})

    def pending(self) -> list[str]:
        if not self.dir.is_dir():
            return []
        return sorted(p.stem for p in self.dir.glob("*.json")
                      if not p.name.endswith(".status.json") and protocol.DEVICE_ID_RE.match(p.stem))

    def load(self, device_id: str) -> Optional[dict]:
        try:
            data = json.loads(self._path(device_id, ".json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if isinstance(data, dict) and isinstance(data.get("profile"), str) else None

    def drop(self, device_id: str) -> None:
        self._path(device_id, ".json").unlink(missing_ok=True)

    def report(self, device_id: str, **status) -> None:
        self._write(self._path(device_id, ".status.json"), {**status, "at": time.time()})

    def status(self, device_id: str) -> Optional[dict]:
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
