"""Process-local registry of live gadget adapters (one per served profile).

Tools run on agent worker threads and need the adapter that owns the device
connections; this module is the meeting point and imports nothing from Hermes.
"""

from __future__ import annotations

import threading
from typing import Any

_lock = threading.Lock()
_adapters: dict[str, Any] = {}


def register(profile: str, adapter: Any) -> None:
    with _lock:
        _adapters[profile] = adapter


def unregister(profile: str, adapter: Any) -> None:
    with _lock:
        if _adapters.get(profile) is adapter:
            del _adapters[profile]


def active(profile: str | None = None) -> Any | None:
    """The adapter serving ``profile``; with no profile, the only one running."""
    with _lock:
        if profile and profile in _adapters:
            return _adapters[profile]
        if len(_adapters) == 1:
            return next(iter(_adapters.values()))
        return _adapters.get("default")


def any_active() -> bool:
    with _lock:
        return bool(_adapters)
