#!/usr/bin/env python3
"""Fail when the app image leaves too little room in its flash slot.

ESP-IDF only stops a build whose app no longer fits. Running out of room
later is worse: the slot sizes are fixed on every board already flashed, so
a release that outgrows them can only be installed over USB. Keep a margin.

PlatformIO builds run this after linking (see pio_checks.py). For idf.py:

    python tools/check_size.py --app build/hermes_gadget.bin \\
        --partitions build/partition_table/partition-table.bin
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import NamedTuple

ENTRY = struct.Struct("<2sBBII16sI")
MAGIC = b"\xaa\x50"
TYPE_APP = 0x00


class Partition(NamedTuple):
    type: int
    subtype: int
    offset: int
    size: int
    label: str


def partitions(table: bytes) -> list[Partition]:
    """Every entry of a binary partition table, in table order."""
    entries = []
    for off in range(0, len(table) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, subtype, offset, size, label, _flags = ENTRY.unpack_from(table, off)
        if magic != MAGIC:
            break  # 0xEBEB (MD5 entry) or 0xFFFF ends the table
        entries.append(Partition(ptype, subtype, offset, size, label.rstrip(b"\0").decode(errors="replace")))
    return entries


def app_slots(table: bytes) -> list[tuple[str, int]]:
    """(label, size) of every app partition in a binary partition table."""
    return [(p.label, p.size) for p in partitions(table) if p.type == TYPE_APP]


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--app", required=True, type=Path, help="The app image (.bin)")
    p.add_argument("--partitions", required=True, type=Path, help="The binary partition table")
    p.add_argument("--min-free", type=float, default=10.0, help="Percent of the smallest app slot to keep free")
    args = p.parse_args(argv)

    slots = app_slots(args.partitions.read_bytes())
    if not slots:
        print(f"check_size: no app partition in {args.partitions}", file=sys.stderr)
        return 2
    label, slot = min(slots, key=lambda s: s[1])
    size = args.app.stat().st_size
    free = 100.0 * (slot - size) / slot
    summary = f"app {size / 1024:.0f} KB in the {slot / 1024:.0f} KB '{label}' slot, {free:.0f}% free"
    if free < args.min_free:
        print(f"check_size: {summary}; keep at least {args.min_free:.0f}% free. "
              "Make the app smaller or its partitions bigger.", file=sys.stderr)
        return 1
    print(f"check_size: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
