"""`hermes plugins install` from GitHub: Hermes's install scan must let the plugin through.

Hermes scans the directory it installs, with its plugin scanner, before installing it. A
"caution" verdict needs confirmation, and a "dangerous" one blocks the install even with --force.
This runs that scan on the `plugin` directory, the one the documented commands install. The
repository root is not a plugin: its scan includes the Raspberry Pi installer and is blocked.

Run with a Python that can import Hermes, e.g.:
    HERMES_AGENT_DIR=../hermes-agent ../hermes-agent/.venv/Scripts/python -m pytest tests/test_plugin_install_hermes.py
"""

from __future__ import annotations

import shutil

from conftest import REPO, requires_hermes

pytestmark = requires_hermes

SOURCE = "https://github.com/Adolanium/hermes-gadget-sdk.git#plugin"


def test_hermes_lets_the_plugin_install_from_github(tmp_path):
    from tools.plugin_guard import format_scan_report, scan_plugin, should_allow_plugin_install

    # What a clone holds: the tracked files, no bytecode from this test run.
    plugin = shutil.copytree(REPO / "plugin", tmp_path / "plugin", ignore=shutil.ignore_patterns("__pycache__"))
    result = scan_plugin(plugin, source=SOURCE)
    allowed, reason = should_allow_plugin_install(result)
    assert result.trust_level == "community"
    # Allowed without --force or a confirmation prompt.
    assert allowed is True, f"{reason}\n{format_scan_report(result)}"
