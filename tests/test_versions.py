"""One version for the firmware, the Python package and the plugin (tools/check_versions.py)."""

from __future__ import annotations

import importlib.util
import shutil
import sys

from conftest import REPO

spec = importlib.util.spec_from_file_location("check_versions", REPO / "tools" / "check_versions.py")
check_versions = importlib.util.module_from_spec(spec)
sys.modules["check_versions"] = check_versions
spec.loader.exec_module(check_versions)


def test_every_file_carries_the_same_version():
    found = check_versions.versions()
    assert None not in found.values(), found
    assert len(set(found.values())) == 1, found
    assert check_versions.main([]) == 0


def _copy(tmp_path):
    for rel in check_versions.FILES:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, tmp_path / rel)
    return tmp_path


def test_a_file_left_behind_fails(tmp_path, capsys):
    repo = _copy(tmp_path)
    version = check_versions.versions(repo)["plugin/plugin.yaml"]
    plugin = repo / "plugin" / "plugin.yaml"
    plugin.write_text(plugin.read_text(encoding="utf-8").replace(f"version: {version}", "version: 9.9.9", 1),
                      encoding="utf-8")
    assert check_versions.main(["--repo", str(repo)]) == 1
    assert "plugin/plugin.yaml 9.9.9" in capsys.readouterr().err


def test_the_tag_must_match(tmp_path, capsys):
    repo = _copy(tmp_path)
    version = check_versions.versions(repo)["pyproject.toml"]
    assert check_versions.main(["--repo", str(repo), "--tag", f"v{version}"]) == 0
    assert check_versions.main(["--repo", str(repo), "--tag", "v99.0.0"]) == 1
    assert "the tag v99.0.0 doesn't match" in capsys.readouterr().err


def test_a_missing_version_is_reported(tmp_path, capsys):
    repo = _copy(tmp_path)
    (repo / "python" / "hermes_gadget" / "__init__.py").write_text('"""No version here."""\n', encoding="utf-8")
    assert check_versions.main(["--repo", str(repo)]) == 1
    assert "python/hermes_gadget/__init__.py: no version found" in capsys.readouterr().err
