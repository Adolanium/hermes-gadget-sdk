"""The plugin says what it needs from Hermes, and fails clearly when it is missing."""

from __future__ import annotations

import ast
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


def _module_level_imports(node):
    for child in ast.iter_child_nodes(node):
        if isinstance(child, (ast.Import, ast.ImportFrom)):
            yield child
        elif not isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
            yield from _module_level_imports(child)


def test_every_module_level_hermes_import_is_listed():
    """A module-level import the table lacks fails right after compat.check passes, without its message."""
    own_dependencies = {"websockets", "PIL"}  # pyproject.toml: websockets, and Pillow in the images extra
    examined, unlisted = 0, []
    for path in sorted((REPO / "plugin").glob("*.py")):
        for node in _module_level_imports(ast.parse(path.read_text(encoding="utf-8"))):
            if isinstance(node, ast.Import):
                imports = [(alias.name, ()) for alias in node.names]
            elif node.level == 0:
                imports = [(node.module, tuple(alias.name for alias in node.names))]
            else:
                continue
            for module, names in imports:
                if module.split(".")[0] in sys.stdlib_module_names | own_dependencies:
                    continue
                examined += 1
                listed = compat.IMPORTS.get(module)
                if listed is None or not set(names) <= set(listed):
                    unlisted.append(f"{path.name}: {module} {[n for n in names if n not in (listed or ())]}")
    assert examined, "no Hermes import found in plugin/*.py"
    assert not unlisted, f"not in compat.IMPORTS: {unlisted}"


@requires_hermes
def test_the_checked_out_hermes_is_compatible():
    assert compat.missing() == []


def test_approval_timeout_falls_back_to_the_default_without_hermes(monkeypatch):
    monkeypatch.setitem(sys.modules, "gateway.platforms.base_exec_approval", None)  # its import now fails
    assert compat.approval_timeout_s() == 300


# Packages the plugin may import at module level that are not Hermes (none today; they are imported
# lazily where they are optional).
_NOT_HERMES = frozenset({"websockets", "PIL"})


def _module_level_hermes_imports():
    """Each top-level `from X import Y` / `import X` in plugin/*.py that names neither the standard
    library, the plugin itself nor a known third-party package: those come from Hermes."""
    import ast

    found = []
    for path in sorted((REPO / "plugin").glob("*.py")):
        for node in ast.parse(path.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
                modules = [(node.module, tuple(alias.name for alias in node.names))]
            elif isinstance(node, ast.Import):
                modules = [(alias.name, ()) for alias in node.names]
            else:
                continue
            for module, names in modules:
                top = module.split(".")[0]
                if top in sys.stdlib_module_names or top in _NOT_HERMES or top == "__future__":
                    continue
                found.append((path.name, module, names))
    return found


def test_every_module_level_hermes_import_is_listed_in_compat_imports():
    # compat.check() runs before the adapter imports: a Hermes name it doesn't list would fail
    # later as a bare ImportError instead of the plugin's compatibility message.
    found = _module_level_hermes_imports()
    assert found, "no module-level Hermes imports found: the scan is broken"
    gaps = []
    for file, module, names in found:
        listed = compat.IMPORTS.get(module)
        if listed is None:
            gaps.append(f"{file}: {module} is not in compat.IMPORTS")
            continue
        missing = sorted(set(names) - set(listed))
        if missing:
            gaps.append(f"{file}: {module} {missing} not in compat.IMPORTS")
    assert not gaps, "\n".join(gaps)
