#!/usr/bin/env python3
"""Copy an idf.py build of a board to where package_release.py reads a PlatformIO build.

PlatformIO has no platform for some chips (the ESP32-P4), so their boards (IDF_BOARDS in
package_release.py) are built with idf.py. This copies such a build's files to
.pio/build/<board>/ and its sdkconfig to sdkconfig.<board>, the names PlatformIO uses:

    idf.py -B build/crowpanel-p4-101 ... build
    python tools/stage_idf_build.py crowpanel-p4-101 build/crowpanel-p4-101
    python tools/package_release.py --board crowpanel-p4-101 --out dist

With --notices DIR it also copies the notices of the components and ESP-IDF the build used to
DIR/<board>/, for package_release.py --notices DIR when the release is packaged elsewhere.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_release import BOOTLOADER_OFFSET, IDF_BOARDS, PROJECT_DIR, PackageError, build_notices  # noqa: E402

# flasher_args.json's entry for each file package_release.py reads from .pio/build/<board>/
FLASHER_ENTRIES = {"bootloader": "bootloader.bin", "partition-table": "partitions.bin",
                   "otadata": "ota_data_initial.bin", "app": "firmware.bin"}


def stage(project: Path, board: str, build: Path, notices: Path | None = None) -> Path:
    if board not in IDF_BOARDS:
        raise PackageError(f"{board} is not in IDF_BOARDS")
    try:
        flasher = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise PackageError(f"{build} has no flasher_args.json; build {board} with idf.py first") from None
    chip = flasher.get("extra_esptool_args", {}).get("chip", "")
    bootloader = int(flasher["bootloader"]["offset"], 0)
    if bootloader != BOOTLOADER_OFFSET.get(chip, 0):
        raise PackageError(f"ESP-IDF puts the {chip} bootloader at 0x{bootloader:x}, and "
                           f"package_release.py at 0x{BOOTLOADER_OFFSET.get(chip, 0):x}; fix BOOTLOADER_OFFSET")
    out = project / ".pio" / "build" / board
    out.mkdir(parents=True, exist_ok=True)
    for entry, name in FLASHER_ENTRIES.items():
        shutil.copyfile(build / flasher[entry]["file"], out / name)
    shutil.copyfile(build / "sdkconfig", project / f"sdkconfig.{board}")
    if notices:
        for source, name in build_notices(project):
            target = notices / board / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("board", help="A board in package_release.py's IDF_BOARDS")
    p.add_argument("build", type=Path, help="The idf.py build directory (-B)")
    p.add_argument("--project", type=Path, default=PROJECT_DIR, help="The firmware/esp32 directory")
    p.add_argument("--notices", type=Path, help="Also copy the build's notices under this directory")
    args = p.parse_args(argv)
    try:
        out = stage(args.project, args.board, args.build, args.notices)
    except PackageError as exc:
        print(f"stage_idf_build: {exc}", file=sys.stderr)
        return 1
    print(f"stage_idf_build: {args.board} staged in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
