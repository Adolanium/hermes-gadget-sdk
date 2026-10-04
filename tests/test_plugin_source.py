"""The plugin's source, as Hermes's install scan sees it."""

from __future__ import annotations

import unicodedata

from conftest import REPO


def test_plugin_source_has_no_invisible_characters():
    # Hermes blocks a community plugin whose files contain invisible characters (zero-width spaces,
    # bidi controls, ...) as possible hidden instructions. Write them as escapes ("\u200b").
    found = []
    for path in sorted((REPO / "plugin").rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            for char in line:
                if unicodedata.category(char) in ("Cf", "Zl", "Zp") or (
                        unicodedata.category(char) == "Zs" and char != " "):
                    found.append(f"{path.relative_to(REPO)}:{number}: U+{ord(char):04X} {unicodedata.name(char, '?')}")
    assert not found, "\n".join(found)
