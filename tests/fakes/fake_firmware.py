"""A stand-in for firmware.bin: the headers and tags the update path reads, then filler."""

from __future__ import annotations

import random
import struct


def fake_image(*, board: str | None = "sim-320x240", version: str = "0.2.0", project: str = "hermes_gadget",
               size: int = 150_000, seed: int = 1) -> bytes:
    data = bytearray(random.Random(seed).randbytes(size))
    data[0] = 0xE9                                   # esp_image_header_t.magic
    data[1:32] = bytes(31)
    struct.pack_into("<I", data, 32, 0xABCD5432)     # esp_app_desc_t.magic_word
    data[36:256] = bytes(220)
    data[48:48 + len(version)] = version.encode()    # esp_app_desc_t.version
    data[80:80 + len(project)] = project.encode()    # esp_app_desc_t.project_name
    if board:
        tag = b"HGBOARD=" + board.encode() + b"\0"   # firmware/esp32/main/board.cpp
        data[4096:4096 + len(tag)] = tag
    return bytes(data)
