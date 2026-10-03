"""RGB565 framebuffer conversion and a minimal PNG writer (stdlib only)."""

from __future__ import annotations

import struct
import zlib
from array import array

_LUT: list[bytes] | None = None


def _lut() -> list[bytes]:
    global _LUT
    if _LUT is None:
        table = []
        for v in range(65536):
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            table.append(bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))))
        _LUT = table
    return _LUT


def rgb565_to_rgb888(raw: bytes) -> bytes:
    """``raw`` is little-endian RGB565."""
    px = array("H")
    px.frombytes(raw)
    lut = _lut()
    return b"".join(lut[v] for v in px)


def encode_png(rgb: bytes, width: int, height: int) -> bytes:
    stride = width * 3
    rows = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")


def encode_ppm(rgb: bytes, width: int, height: int) -> bytes:
    return b"P6\n%d %d\n255\n" % (width, height) + rgb
