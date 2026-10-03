"""Durable device registry: enrolled device keys and pending pairing codes.

Lives in the plugin's data directory (``<hermes home>/plugin-data/gadget/``),
never in the plugin install tree. Device keys are generated on the device and
enrolled on first contact (trust on first use); later connections prove
possession with an HMAC challenge, so the key crosses the network only once.
"""

from __future__ import annotations

import base64
import json
import os
import threading
import time
from pathlib import Path
from typing import Any


class DeviceStore:
    FILENAME = "devices.json"

    def __init__(self, directory: Path | str):
        self._dir = Path(directory)
        self._path = self._dir / self.FILENAME
        self._lock = threading.Lock()
        self._data: dict[str, Any] = {"devices": {}, "pairing": {}}
        self._load()

    @property
    def path(self) -> Path:
        return self._path

    def _load(self) -> None:
        try:
            raw = json.loads(self._path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, ValueError):
            # Keep the unreadable file for inspection; start empty.
            return
        if isinstance(raw, dict):
            self._data["devices"] = dict(raw.get("devices") or {})
            self._data["pairing"] = dict(raw.get("pairing") or {})

    def _save(self) -> None:
        self._dir.mkdir(parents=True, exist_ok=True)
        tmp = self._path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self._data, indent=2, sort_keys=True), encoding="utf-8")
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        os.replace(tmp, self._path)

    # -- enrollment -----------------------------------------------------------

    def key_for(self, device_id: str) -> bytes | None:
        with self._lock:
            rec = self._data["devices"].get(device_id)
        if not rec:
            return None
        try:
            return base64.b64decode(rec["key"])
        except (KeyError, ValueError):
            return None

    def enroll(self, device_id: str, key: bytes, *, name: str = "", board: str = "") -> None:
        now = time.time()
        with self._lock:
            self._data["devices"][device_id] = {
                "key": base64.b64encode(key).decode(),
                "name": name,
                "board": board,
                "enrolled_at": now,
                "last_seen": now,
            }
            self._save()

    def touch(self, device_id: str, *, name: str = "", board: str = "") -> None:
        with self._lock:
            rec = self._data["devices"].get(device_id)
            if rec is None:
                return
            rec["last_seen"] = time.time()
            if name:
                rec["name"] = name
            if board:
                rec["board"] = board
            self._save()

    def forget(self, device_id: str) -> bool:
        with self._lock:
            removed = self._data["devices"].pop(device_id, None) is not None
            self._data["pairing"].pop(device_id, None)
            if removed:
                self._save()
            return removed

    def devices(self) -> dict[str, dict[str, Any]]:
        with self._lock:
            return {k: {kk: vv for kk, vv in v.items() if kk != "key"} for k, v in self._data["devices"].items()}

    # -- pairing codes ----------------------------------------------------------

    def remember_pairing(self, device_id: str, code: str, command: str, ttl_s: float) -> None:
        with self._lock:
            self._data["pairing"][device_id] = {"code": code, "command": command, "expires_at": time.time() + ttl_s}
            self._save()

    def pairing_for(self, device_id: str) -> tuple[str, str] | None:
        with self._lock:
            rec = self._data["pairing"].get(device_id)
            if not rec:
                return None
            if rec.get("expires_at", 0) < time.time():
                self._data["pairing"].pop(device_id, None)
                self._save()
                return None
            return rec["code"], rec["command"]

    def clear_pairing(self, device_id: str) -> None:
        with self._lock:
            if self._data["pairing"].pop(device_id, None) is not None:
                self._save()
