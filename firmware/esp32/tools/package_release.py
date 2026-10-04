#!/usr/bin/env python3
"""Turn PlatformIO builds into release files: one image per board, its app, checksums and a manifest.

For each board (a PlatformIO environment) it writes:

    hermes-gadget-<board>-<version>.bin      bootloader, partition table, OTA data and app, from
                                             address 0. Flashed at 0x0 it also erases the
                                             device's settings, so the device pairs again.
    hermes-gadget-<board>-<version>-app.bin  the app alone, for `hermes gadget update`.

plus SHA256SUMS and manifest.json, which describes each build for the browser installer: the
chip, flash size and PSRAM it needs, and the settings area to leave alone so a reinstalled
device keeps its Wi-Fi, server and key.

    python tools/package_release.py --all --out dist
    python tools/package_release.py --board esp32s3-breadboard --out dist --expect-version 0.1.0
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_size import TYPE_APP, partitions  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
PROJECT_NAME = "hermes_gadget"      # esp_app_desc_t.project_name of every Hermes Gadget image
BOARD_TAG = b"HGBOARD="             # the board name the firmware reports (main/board.cpp)
INSTALLER_URL = "https://adolanium.github.io/hermes-gadget-sdk/"
TYPE_DATA, SUBTYPE_OTA, SUBTYPE_NVS = 0x01, 0x00, 0x02
# Where the second-stage bootloader goes; every chip after the ESP32-S2 starts at 0.
BOOTLOADER_OFFSET = {"esp32": 0x1000, "esp32s2": 0x1000}
CHIP_NAMES = {"esp32": "ESP32", "esp32s2": "ESP32-S2", "esp32s3": "ESP32-S3", "esp32c3": "ESP32-C3",
              "esp32c6": "ESP32-C6", "esp32h2": "ESP32-H2"}
BUILD_FILES = ("bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin")


class PackageError(Exception):
    pass


@dataclass
class Build:
    board: str          # the PlatformIO environment
    image: bytes
    app: bytes
    version: str
    image_board: str    # the board name inside the image, which the device reports to Hermes
    chip: str
    flash_size: str
    psram: str | None   # "octal", "quad" or None
    settings: tuple[int, int] | None  # the NVS partition: (offset, size)
    meta: dict


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def read_sdkconfig(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if m := re.match(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$", line.strip()):
            values[m.group(1)] = m.group(2).strip().strip('"')
    return values


def environments(project: Path) -> dict[str, str]:
    """Every board platformio.ini defines, in file order, mapped to its directory under boards/.

    The directory comes from the environment's SDKCONFIG_DEFAULTS, since a board's environment
    and directory names needn't match.
    """
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(project / "platformio.ini", encoding="utf-8")
    boards = {}
    for section in ini.sections():
        if section.startswith("env:"):
            env = section.split(":", 1)[1]
            extra_args = ini.get(section, "board_build.cmake_extra_args", fallback="")
            m = re.search(r"boards/([^/;\"]+)/sdkconfig\.defaults", extra_args)
            boards[env] = m.group(1) if m else env
    return boards


def app_identity(app: bytes) -> tuple[str, str]:
    """(version, board) from a Hermes Gadget app image."""
    if len(app) < 256 or app[0] != 0xE9 or struct.unpack_from("<I", app, 32)[0] != 0xABCD5432:
        raise PackageError("firmware.bin is not an ESP-IDF app image")
    project = _cstr(app[80:112])
    if project != PROJECT_NAME:
        raise PackageError(f"firmware.bin is {project!r}, not Hermes Gadget firmware")
    tag = app.find(BOARD_TAG)
    if tag < 0:
        raise PackageError("firmware.bin carries no board name (HGBOARD=...)")
    return _cstr(app[48:80]), _cstr(app[tag + len(BOARD_TAG): tag + len(BOARD_TAG) + 64])


def merge(parts: list[tuple[int, bytes]]) -> bytes:
    """Place each (address, data) in one image, gaps erased (0xFF) as on blank flash."""
    image = bytearray(b"\xff" * max(offset + len(data) for offset, data in parts))
    end = 0
    for offset, data in sorted(parts):
        if offset < end:
            raise PackageError(f"image parts overlap at 0x{offset:x}")
        image[offset:offset + len(data)] = data
        end = offset + len(data)
    return bytes(image)


def build_for(project: Path, board: str, board_dir: str) -> Build:
    build_dir = project / ".pio" / "build" / board
    missing = [name for name in BUILD_FILES if not (build_dir / name).is_file()]
    sdkconfig = project / f"sdkconfig.{board}"
    if missing or not sdkconfig.is_file():
        raise PackageError(f"{board} isn't built (missing {', '.join(missing) or sdkconfig.name}); "
                           f"run: pio run -e {board}")
    def read(name: str) -> bytes:
        return (build_dir / name).read_bytes()

    sdk = read_sdkconfig(sdkconfig)
    target = sdk.get("CONFIG_IDF_TARGET", "esp32")
    table = read("partitions.bin")
    entries = partitions(table)
    apps = sorted((p for p in entries if p.type == TYPE_APP), key=lambda p: p.offset)
    otadata = next((p for p in entries if p.type == TYPE_DATA and p.subtype == SUBTYPE_OTA), None)
    nvs = next((p for p in entries if p.type == TYPE_DATA and p.subtype == SUBTYPE_NVS), None)
    if not apps:
        raise PackageError(f"{board}: the partition table has no app partition")
    app = read("firmware.bin")
    if len(app) > apps[0].size:
        raise PackageError(f"{board}: the app ({len(app)} bytes) doesn't fit '{apps[0].label}' ({apps[0].size})")
    version, image_board = app_identity(app)

    parts = [(BOOTLOADER_OFFSET.get(target, 0), read("bootloader.bin")),
             (int(sdk.get("CONFIG_PARTITION_TABLE_OFFSET", "0x8000"), 0), table),
             (apps[0].offset, app)]
    if otadata:
        parts.append((otadata.offset, read("ota_data_initial.bin")))
    meta_path = project / "boards" / board_dir / "board.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else {}
    psram = None
    if sdk.get("CONFIG_SPIRAM") == "y":
        psram = "octal" if sdk.get("CONFIG_SPIRAM_MODE_OCT") == "y" else "quad"
    return Build(
        board=board, image=merge(parts), app=app, version=version, image_board=image_board,
        chip=CHIP_NAMES.get(target, target.upper()), flash_size=sdk.get("CONFIG_ESPTOOLPY_FLASHSIZE", ""),
        psram=psram, settings=(nvs.offset, nvs.size) if nvs else None, meta=meta)


def _file(path: Path, data: bytes) -> dict:
    path.write_bytes(data)
    return {"path": path.name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def write_release(builds: list[Build], out: Path) -> dict:
    versions = sorted({b.version for b in builds})
    if len(versions) != 1:
        raise PackageError(f"the boards report different versions: {', '.join(versions)}")
    version = versions[0]
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"name": "Hermes Gadget", "version": version, "builds": []}
    for b in builds:
        stem = f"hermes-gadget-{b.board}-{version}"
        manifest["builds"].append({
            "board": b.board,
            "image_board": b.image_board,
            "title": b.meta.get("title") or b.board,
            "summary": b.meta.get("summary", ""),
            "docs": b.meta.get("docs", ""),
            "ready_made": bool(b.meta.get("ready_made")),  # an all-in-one board: nothing to wire
            "chip": b.chip,
            "flash_size": b.flash_size,
            "psram": b.psram,
            "image": _file(out / f"{stem}.bin", b.image),
            "app": _file(out / f"{stem}-app.bin", b.app),
            "settings": {"offset": b.settings[0], "size": b.settings[1]} if b.settings else None,
        })
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    sums = [f"{f['sha256']}  {f['path']}" for build in manifest["builds"] for f in (build["image"], build["app"])]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    return manifest


def release_notes(manifest: dict) -> str:
    version = manifest["version"]
    rows = "\n".join(f"| {b['title']} | `{b['image']['path']}` | `{b['app']['path']}` |" for b in manifest["builds"])
    chips = sorted({b["chip"].lower().replace("-", "") for b in manifest["builds"]})
    return f"""Hermes Gadget firmware {version}.

**Install from your browser:** {INSTALLER_URL} (Chrome or Edge, over USB). It checks the board before \
writing, and a device you reinstall keeps its settings.

| Board | Full image | Over-the-air update |
|---|---|---|
{rows}

**Flash with esptool:** `esptool.py --chip {chips[0] if len(chips) == 1 else '<chip>'} write_flash 0x0 \
hermes-gadget-<board>-{version}.bin`. This also erases the device's settings, so it pairs again as a new device.

**Update a running device:** `hermes gadget update <device> hermes-gadget-<board>-{version}-app.bin`.

Checksums are in `SHA256SUMS`.
"""


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    which = p.add_mutually_exclusive_group(required=True)
    which.add_argument("--board", action="append", help="A PlatformIO environment (repeatable)")
    which.add_argument("--all", action="store_true", help="Every environment in platformio.ini")
    p.add_argument("--out", required=True, type=Path, help="Directory for the release files")
    p.add_argument("--project", type=Path, default=PROJECT_DIR, help="The firmware/esp32 directory")
    p.add_argument("--expect-version", help="Fail unless the firmware reports this version (e.g. from the tag)")
    p.add_argument("--notes", type=Path, help="Also write release notes (Markdown) to this file")
    args = p.parse_args(argv)

    try:
        known = environments(args.project)
        boards = list(known) if args.all else args.board
        unknown = [board for board in boards if board not in known]
        if unknown:
            raise PackageError(f"not in platformio.ini: {', '.join(unknown)}")
        builds = [build_for(args.project, board, known[board]) for board in boards]
        if args.expect_version and builds[0].version != args.expect_version:
            raise PackageError(f"the firmware reports {builds[0].version}, not {args.expect_version}; "
                               "update PROJECT_VER in firmware/esp32/CMakeLists.txt")
        manifest = write_release(builds, args.out)
    except PackageError as exc:
        print(f"package_release: {exc}", file=sys.stderr)
        return 1
    if args.notes:
        args.notes.write_text(release_notes(manifest), encoding="utf-8")
    for build in manifest["builds"]:
        print(f"package_release: {build['image']['path']} ({build['image']['size'] // 1024} KB), "
              f"{build['app']['path']} ({build['app']['size'] // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
