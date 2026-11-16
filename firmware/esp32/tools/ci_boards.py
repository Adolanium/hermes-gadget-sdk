#!/usr/bin/env python3
"""Pick the boards a pull request's firmware build has to cover.

CI builds every board on a push to main. A pull request builds only what its diff can affect:

- a file under boards/<board>/ selects that board;
- a changed line inside `#if CONFIG_HG_BOARD_...` in a source file selects the boards that
  condition names, and so does a changed line in Kconfig.projbuild or platformio.ini that names one;
- any other change to the firmware's sources is shared code, which selects the boards in SHARED:
  one per chip family and display path, so a break in shared code shows without building all;
- a change to the build itself (ALL_PATHS) selects every board.

    python tools/ci_boards.py --base <sha>      # the boards for the diff base...HEAD
    python tools/ci_boards.py                   # every board

Prints {"pio": [...], "idf": [...]}: PlatformIO environments, and the boards built with idf.py.
"""

from __future__ import annotations

import argparse
import configparser
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_release import PROJECT_DIR, environments  # noqa: E402

REPO = PROJECT_DIR.parent.parent
FIRMWARE = "firmware/esp32/"
# Built when shared code changes: the three boards branch protection once named, the only classic
# ESP32, and one ESP32-P4.
SHARED = ("esp32s3-breadboard", "esp32s3-touch-amoled-175", "esp32s3-lcd-154", "esp32-2432s028-cyd",
          "crowpanel-p4-101")
# A change here can break any board in a way no subset shows.
ALL_PATHS = re.compile(r"^(\.github/workflows/ci\.yml|firmware/esp32/(tools/|components/|sdkconfig\.defaults$|"
                       r"partitions[^/]*\.csv$|CMakeLists\.txt$|main/idf_component\.yml$|dependencies\.lock$))")
# Sources compiled into a board's image. firmware/tests and firmware/sim are host-only.
SOURCE_PATHS = re.compile(r"^firmware/(core/|drivers/|esp32/main/|esp32/platformio\.ini$)")
SYMBOL = re.compile(r"HG_BOARD_[A-Z0-9_]+")
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")


def boards(project: Path) -> dict[str, dict]:
    """Every board: its name in CI, its directory under boards/, its Kconfig symbol and how it is built."""
    found = {}
    envs = {directory: env for env, directory in environments(project).items()}
    for defaults in sorted((project / "boards").glob("*/sdkconfig.defaults")):
        directory = defaults.parent.name
        m = re.search(r"^CONFIG_(HG_BOARD_[A-Z0-9_]+)=y", defaults.read_text(encoding="utf-8"), re.M)
        name = envs.get(directory, directory)
        found[name] = {"dir": directory, "symbol": m.group(1) if m else None,
                       "build": "pio" if directory in envs else "idf"}
    return found


def _named(condition: str) -> set[str]:
    """The boards a condition selects. A negated one selects every other board, so it names none."""
    return set() if "!" in condition else set(SYMBOL.findall(condition))


def line_symbols(text: str) -> list[set[str] | None]:
    """For each line of a source file: the board symbols of the conditionals around it, or None
    when no enclosing conditional names a board (shared code). A conditional's own line counts
    as inside it."""
    out: list[set[str] | None] = []
    stack: list[set[str]] = []  # per open #if: the symbols of the branch the lines are in
    for line in text.splitlines():
        m = CONDITIONAL.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(set() if kind == "ifndef" else _named(rest))
            elif kind == "elif" and stack:
                stack[-1] = _named(rest)
            elif kind == "else" and stack:
                stack[-1] = set()  # every board the chain did not name: shared
            elif kind == "endif" and stack:
                named = set().union(*stack)
                stack.pop()
                out.append(named or None)
                continue
        named = set().union(*stack) if stack else set()
        out.append(named or None)
    return out


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], check=True, capture_output=True,
                          text=True, encoding="utf-8").stdout


def changed_lines(base: str, path: str) -> list[int]:
    """1-based line numbers in HEAD's copy of path that the diff added or changed; a pure deletion
    counts as the line it was removed before."""
    lines = []
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", _git("diff", "-U0", f"{base}...HEAD", "--", path), re.M):
        start, count = int(m.group(1)), int(m.group(2) or 1)
        lines += range(start, start + count) if count else [max(start, 1)]
    return lines


def _ini_sections(text: str) -> dict[str, dict]:
    ini = configparser.ConfigParser(interpolation=None)
    ini.read_string(text)
    return {s: dict(ini[s]) for s in ini.sections()}


def select(project: Path, base: str | None, files: list[str] | None = None) -> dict[str, list[str]]:
    every = boards(project)

    def result(names) -> dict[str, list[str]]:
        return {kind: [n for n in every if n in names and every[n]["build"] == kind] for kind in ("pio", "idf")}

    if base is None:
        return result(set(every))
    if files is None:
        files = _git("diff", "--name-only", f"{base}...HEAD").split()
    if any(ALL_PATHS.match(f) for f in files):
        return result(set(every))
    by_symbol = {b["symbol"]: name for name, b in every.items() if b["symbol"]}
    by_dir = {b["dir"]: name for name, b in every.items()}
    picked: set[str] = set()
    shared = False
    for path in files:
        m = re.match(r"^firmware/esp32/boards/([^/]+)/", path)
        if m:
            if m.group(1) in by_dir:  # a removed board has nothing to build
                picked.add(by_dir[m.group(1)])
            continue
        if not SOURCE_PATHS.match(path):
            continue
        source = REPO / path
        if not source.is_file():  # deleted: whatever included it is shared code
            shared = True
            continue
        if path.endswith("platformio.ini"):
            old = _ini_sections(_git("show", f"{base}:{path}"))
            new = _ini_sections(source.read_text(encoding="utf-8"))
            for section in set(old) | set(new):
                if old.get(section) == new.get(section):
                    continue
                if section.startswith("env:") and section[4:] in every:
                    picked.add(section[4:])
                elif not section.startswith("env:"):
                    return result(set(every))  # [env] or [platformio]: every environment
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        symbols = line_symbols(text) if path.endswith((".cpp", ".hpp", ".c", ".h")) else None
        for number in changed_lines(base, path):
            if number > len(lines):
                shared = True
                continue
            named = symbols[number - 1] if symbols else set(SYMBOL.findall(lines[number - 1]))
            known = {by_symbol[s] for s in named or () if s in by_symbol}
            if known:
                picked |= known
            else:
                shared = True
    if shared:
        picked |= set(SHARED)
    return result(picked)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--base", help="Compare HEAD with this commit; without it, every board")
    p.add_argument("--project", type=Path, default=PROJECT_DIR, help="The firmware/esp32 directory")
    args = p.parse_args(argv)
    print(json.dumps(select(args.project, args.base)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
