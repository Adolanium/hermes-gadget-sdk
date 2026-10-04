"""`hermes plugins install` from GitHub: Hermes's install scan must let the plugin through.

Hermes scans a plugin before installing it, and blocks a community source with any high-severity
finding unless the user adds --force. This runs that scan, as installing from this repository does.

Run with a Python that can import Hermes, e.g.:
    HERMES_AGENT_DIR=../hermes-agent ../hermes-agent/.venv/Scripts/python -m pytest tests/test_plugin_install_hermes.py
"""

from __future__ import annotations

import shutil

from conftest import REPO, requires_hermes

pytestmark = requires_hermes

SOURCE = "https://github.com/Adolanium/hermes-gadget-sdk.git#plugin"


def test_hermes_lets_the_plugin_install_from_github(tmp_path):
    from tools.skills_guard import format_scan_report, scan_skill, should_allow_install

    # What a clone holds: the tracked files, no bytecode from this test run.
    plugin = shutil.copytree(REPO / "plugin", tmp_path / "plugin", ignore=shutil.ignore_patterns("__pycache__"))
    result = scan_skill(plugin, source=SOURCE)
    allowed, reason = should_allow_install(result)
    assert result.trust_level == "community"
    assert allowed is True, f"{reason}\n{format_scan_report(result)}"
