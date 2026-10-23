"""Process-local registry of live gadget adapters (one per served profile).

Tools run on agent worker threads and need the adapter that owns the device
connections; this module is the meeting point and imports nothing from Hermes.
"""

from __future__ import annotations

import sys
import threading
import types
from typing import Any, Iterable

# Hermes imports a directory plugin once per profile scope (``hermes_plugins.gadget``, then
# ``hermes_plugins.gadget__home_<digest>``), so module globals would give each profile its own
# empty registry. A profile routed to the default profile's gadgets (gateway.profile_routes)
# must still find the adapter: keep one registry per process, shared by every copy.
_SHARED = "hermes_gadget_runtime_registry"
_fresh = types.ModuleType(_SHARED)
_fresh.lock = threading.Lock()
_fresh.adapters = {}
_fresh.boards = {}  # each profile's dashboard.SessionBoard, from that profile's copy of the plugin
_registry = sys.modules.setdefault(_SHARED, _fresh)  # atomic: two copies importing at once share one
_lock: threading.Lock = _registry.lock
_adapters: dict[str, Any] = _registry.adapters
_boards: dict[str, Any] = _registry.boards


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


def add_board(profile: str, board: Any) -> None:
    with _lock:
        _boards[profile] = board


def boards(profiles: Iterable[str]) -> list[Any]:
    """The session boards of those ``profiles`` whose plugin copy has loaded."""
    with _lock:
        return [_boards[p] for p in profiles if p in _boards]
