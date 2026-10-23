"""The adapter registry is one per process, even though Hermes imports the plugin once per profile.

Hermes loads a directory plugin as ``hermes_plugins.gadget`` for the first profile scope and as
``hermes_plugins.gadget__home_<digest>`` for every other one. A profile routed to the default
profile's gadgets (``gateway.profile_routes``) runs the tools from its own copy, which must still
find the adapter the default profile registered, or the gadget tools hide themselves.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

RUNTIME = Path(__file__).resolve().parents[1] / "plugin" / "runtime.py"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, RUNTIME)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def copies(monkeypatch):
    monkeypatch.delitem(sys.modules, "hermes_gadget_runtime_registry", raising=False)
    default = _load("hermes_plugins.gadget.runtime")
    routed = _load("hermes_plugins.gadget__home_0123456789ab.runtime")
    yield default, routed
    sys.modules.pop("hermes_gadget_runtime_registry", None)


def test_a_routed_profile_finds_the_default_profiles_adapter(copies):
    default, routed = copies
    assert not routed.any_active()
    adapter = object()
    default.register("default", adapter)
    assert routed.any_active()  # the tools' check_fn: the gadget tools stay available
    assert routed.active("nina") is adapter  # no adapter of its own: the only one running
    default.unregister("default", adapter)
    assert not routed.any_active()


def test_a_profile_with_its_own_adapter_still_gets_its_own(copies):
    default, routed = copies
    mine, theirs = object(), object()
    default.register("default", theirs)
    routed.register("lab", mine)
    assert default.active("lab") is mine
    assert routed.active("default") is theirs
    assert routed.active("nina") is theirs  # several running: an unrouted profile falls back to default


def test_the_default_profiles_copy_finds_another_profiles_session_board(copies):
    default, routed = copies
    mine, ops = object(), object()
    default.add_board("default", mine)
    routed.add_board("ops", ops)  # the ops profile's copy registers its hooks on its own board
    assert default.boards(["default", "ops", "lab"]) == [mine, ops]  # lab's copy never loaded
    assert default.boards(["ops"]) == [ops]  # only the profiles asked for
