"""Locations inside a source checkout of the SDK."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def repo_root() -> Path:
    """The SDK checkout this package runs from (python/hermes_gadget/ -> repo)."""
    return Path(__file__).resolve().parents[2]


def plugin_dir() -> Path:
    import hermes_gadget_plugin

    return Path(hermes_gadget_plugin.__file__).resolve().parent


def firmware_dir() -> Path:
    return repo_root() / "firmware"


def host_build_dir() -> Path:
    return repo_root() / "build" / "host"


def sim_library_candidates() -> list[Path]:
    build = host_build_dir()
    if sys.platform == "win32":
        names = ["bin/hgsim.dll", "Release/hgsim.dll", "Debug/hgsim.dll", "hgsim.dll"]
    elif sys.platform == "darwin":
        names = ["libhgsim.dylib", "bin/libhgsim.dylib"]
    else:
        names = ["libhgsim.so", "bin/libhgsim.so"]
    return [build / n for n in names]


def find_sim_library() -> Path | None:
    override = os.environ.get("HGSIM_LIBRARY")
    if override:
        return Path(override)
    for candidate in sim_library_candidates():
        if candidate.exists():
            return candidate
    return None


def default_state_dir() -> Path:
    base = os.environ.get("HERMES_GADGET_HOME")
    return Path(base) if base else Path.home() / ".hermes-gadget"


def hermes_home() -> Path:
    env = os.environ.get("HERMES_HOME")
    return Path(env) if env else Path.home() / ".hermes"
