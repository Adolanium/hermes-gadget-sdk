"""The plugin says what it needs from Hermes, and fails clearly when it is missing."""

from __future__ import annotations

import re
import sys
import types

import pytest

from conftest import REPO, requires_hermes
from hermes_gadget_plugin import compat


def _hermes(overrides: dict[str, object] | None = None, drop: set[str] = frozenset()):
    """A resolver for a fake Hermes that has everything, minus `drop` and with `overrides`."""
    class Base:
        pass

    for hook in compat.ADAPTER_HOOKS:
        setattr(Base, hook, lambda self: None)
    modules = {}
    for module_name, names in compat.IMPORTS.items():
        module = types.SimpleNamespace(**{name: object() for name in names})
        if module_name == "gateway.platforms.base":
            module.BasePlatformAdapter = Base
        modules[module_name] = module
    for dotted, value in (overrides or {}).items():
        module_name, name = dotted.rsplit(".", 1)
        setattr(modules[module_name], name, value)

    def resolve(name):
        if name in drop or name not in modules:
            raise ImportError(f"No module named {name!r}")
        return modules[name]

    return resolve


def test_a_complete_hermes_passes():
    assert compat.missing(_hermes()) == []
    compat.check(_hermes())


def test_a_missing_module_names_the_module():
    gaps = compat.missing(_hermes(drop={"gateway.platforms._shared"}))
    assert gaps == ["gateway.platforms._shared (No module named 'gateway.platforms._shared')"]


def test_a_missing_private_hook_names_the_hook():
    class Base:
        pass

    for hook in compat.ADAPTER_HOOKS:
        if hook != "_is_sender_authorized":
            setattr(Base, hook, lambda self: None)
    gaps = compat.missing(_hermes({"gateway.platforms.base.BasePlatformAdapter": Base}))
    assert gaps == ["BasePlatformAdapter._is_sender_authorized"]


def test_check_raises_with_the_tested_commit():
    with pytest.raises(RuntimeError) as err:
        compat.check(_hermes(drop={"tools.approval"}))
    message = str(err.value)
    assert "tools.approval" in message
    assert compat.TESTED_HERMES_COMMIT[:12] in message
    assert "hermes plugins install" in message


def test_the_workflow_pins_the_same_commit():
    workflow = (REPO / ".github" / "workflows" / "hermes.yml").read_text(encoding="utf-8")
    pinned = re.search(r"(?m)^\s*HERMES_REF: ([0-9a-f]{40})\s*$", workflow)
    assert pinned, "hermes.yml has no HERMES_REF"
    assert pinned.group(1) == compat.TESTED_HERMES_COMMIT


def test_the_adapter_only_uses_listed_private_hooks():
    """Every `self._name` call into the base class is in ADAPTER_HOOKS, so the check stays complete."""
    source = (REPO / "plugin" / "adapter.py").read_text(encoding="utf-8")
    own = set(re.findall(r"(?m)^\s+(?:async )?def (_[a-z_]+)\(", source))
    own |= set(re.findall(r"self\.(_[a-z_]+)\s*(?::[^=\n]+)?=", source))  # plain and annotated assignments
    used = set(re.findall(r"(?:self|super\(\))\.(_[a-z][a-z_]*)\b", source))
    inherited = {name for name in used - own if not name.startswith("__")}
    assert inherited <= set(compat.ADAPTER_HOOKS), sorted(inherited - set(compat.ADAPTER_HOOKS))


@requires_hermes
def test_the_checked_out_hermes_is_compatible():
    assert compat.missing() == []


def test_approval_timeout_falls_back_to_the_default_without_hermes(monkeypatch):
    monkeypatch.setitem(sys.modules, "gateway.platforms.base_exec_approval", None)  # its import now fails
    assert compat.approval_timeout_s() == 300
