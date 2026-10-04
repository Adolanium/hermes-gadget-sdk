#!/usr/bin/env python3
"""Check that every line of the sdkconfig defaults reached the generated sdkconfig.

ESP-IDF keeps building when a defaults line doesn't apply: a misspelled
symbol, an option for another chip, or one whose dependencies aren't met is
dropped silently. It also keeps an existing sdkconfig as it is when the
defaults change later, so a checkout that moved on still builds with old
settings. Either way the firmware runs with settings nobody asked for, which
on a board nobody has tested looks like broken hardware.

CMakeLists.txt runs this after the configuration is generated. It also works
on its own:

    python tools/check_config.py --sdkconfig sdkconfig.esp32s3-breadboard \\
        --defaults "sdkconfig.defaults;boards/esp32s3-breadboard/sdkconfig.defaults"

A later defaults file overrides an earlier one. When a board picks another
option of a choice the base file sets, it says so explicitly with
"# CONFIG_<base option> is not set".
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

_SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
_UNSET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def parse(path: Path) -> list[tuple[int, str, str]]:
    """(line number, symbol, value) for every assignment; "n" for "is not set"."""
    out = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if m := _SET.match(line):
            out.append((number, m.group(1), m.group(2).strip()))
        elif m := _UNSET.match(line):
            out.append((number, m.group(1), "n"))
    return out


def check(sdkconfig: Path, defaults: list[Path]) -> list[str]:
    wanted: dict[str, tuple[str, str]] = {}  # symbol -> (value, "file:line"); later files win
    for path in defaults:
        for number, symbol, value in parse(path):
            wanted[symbol] = (value, f"{path}:{number}")
    actual = {symbol: value for _, symbol, value in parse(sdkconfig)}

    problems = []
    for symbol, (value, where) in wanted.items():
        got = actual.get(symbol)
        if value == "n":
            if got not in (None, "n"):
                problems.append(f"{where}: {symbol} should be off, but the config has {symbol}={got}")
        elif got != value:
            shown = "nothing" if got is None else ("it off" if got == "n" else f"{symbol}={got}")
            problems.append(f"{where}: {symbol}={value}, but the config has {shown}")
    return problems


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--sdkconfig", required=True, type=Path, help="The generated sdkconfig")
    p.add_argument("--defaults", required=True, help="Defaults files in load order, separated by ';'")
    p.add_argument("--root", type=Path, default=Path.cwd(), help="Base for relative --defaults paths")
    args = p.parse_args(argv)

    defaults = [Path(d) if Path(d).is_absolute() else args.root / d
                for d in (d.strip() for d in args.defaults.split(";")) if d]
    missing = [str(d) for d in [args.sdkconfig, *defaults] if not d.is_file()]
    if missing:
        print("check_config: not found: " + ", ".join(missing), file=sys.stderr)
        return 2

    problems = check(args.sdkconfig, defaults)
    if not problems:
        return 0
    print(f"check_config: {args.sdkconfig} doesn't match its defaults:", file=sys.stderr)
    for line in problems:
        print("  " + line, file=sys.stderr)
    print(
        "If the sdkconfig is left over from an older checkout, delete it (or the build directory)\n"
        "and build again. Otherwise the line is misspelled, belongs to another chip, or has an\n"
        "unmet dependency. A board file that picks another option of a choice the base file sets\n"
        "needs '# CONFIG_<base option> is not set' as well.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
