#!/usr/bin/env python3
"""Check that every file carrying the version agrees, and optionally that it matches a release tag.

The firmware, the Python package and the Hermes plugin ship together under one version:

    python tools/check_versions.py               # all files agree
    python tools/check_versions.py --tag v0.2.0  # and match the tag being released
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Each file that carries the version, and the pattern that finds it there.
FILES = {
    "firmware/esp32/CMakeLists.txt": r'set\(PROJECT_VER "([^"]+)"\)',
    "pyproject.toml": r'(?m)^version = "([^"]+)"',
    "python/hermes_gadget/__init__.py": r'(?m)^__version__ = "([^"]+)"',
    "plugin/plugin.yaml": r'(?m)^version: *"?([^"\s]+)"?\s*$',
}


def versions(repo: Path = REPO) -> dict[str, str | None]:
    """The version in each file, or None where it can't be found."""
    found = {}
    for rel, pattern in FILES.items():
        m = re.search(pattern, (repo / rel).read_text(encoding="utf-8"))
        found[rel] = m.group(1) if m else None
    return found


def problems(found: dict[str, str | None], tag: str | None = None) -> list[str]:
    errors = [f"{rel}: no version found" for rel, version in found.items() if version is None]
    if len({version for version in found.values() if version}) > 1:
        errors.append("the versions differ: " + ", ".join(f"{rel} {version}" for rel, version in found.items()))
    if tag is not None:
        wanted = tag.removeprefix("v")
        wrong = [f"{rel} {version}" for rel, version in found.items() if version and version != wanted]
        if wrong:
            errors.append(f"the tag {tag} doesn't match " + ", ".join(wrong))
    return errors


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--tag", help="The release tag, e.g. v0.2.0")
    p.add_argument("--repo", type=Path, default=REPO, help=argparse.SUPPRESS)
    args = p.parse_args(argv)

    found = versions(args.repo)
    errors = problems(found, args.tag)
    if errors:
        for error in errors:
            print(f"check_versions: {error}", file=sys.stderr)
        print(f"check_versions: set one version in {', '.join(FILES)}", file=sys.stderr)
        return 1
    version = next(iter(found.values()))
    print(f"check_versions: {version} in all {len(found)} files" + (f", matching {args.tag}" if args.tag else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
