"""Validate gadget camera results and save them as PNGs on the gateway."""

from __future__ import annotations

import base64
import binascii
import re
import struct
import tempfile
import zlib
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

MAX_WIDTH = 640
MAX_HEIGHT = 512
MAX_RAW_BYTES = MAX_WIDTH * MAX_HEIGHT * 2
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    crc = binascii.crc32(data, binascii.crc32(kind)) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def rgb565_png(pixels: bytes, width: int, height: int) -> bytes:
    """Encode tightly packed little-endian RGB565 pixels as an 8-bit RGB PNG."""
    if not 1 <= width <= MAX_WIDTH or not 1 <= height <= MAX_HEIGHT:
        raise ValueError(f"unsupported camera size {width}x{height}")
    if len(pixels) != width * height * 2:
        raise ValueError("RGB565 payload length doesn't match its dimensions")

    compressor = zlib.compressobj(level=6)
    compressed = bytearray()
    row_size = width * 2
    for y in range(height):
        source = y * row_size
        row = bytearray(1 + width * 3)
        for x in range(width):
            i = source + x * 2
            value = pixels[i] | (pixels[i + 1] << 8)
            r5 = (value >> 11) & 0x1F
            g6 = (value >> 5) & 0x3F
            b5 = value & 0x1F
            j = 1 + x * 3
            row[j] = (r5 << 3) | (r5 >> 2)
            row[j + 1] = (g6 << 2) | (g6 >> 4)
            row[j + 2] = (b5 << 3) | (b5 >> 2)
        compressed.extend(compressor.compress(row))
    compressed.extend(compressor.flush())

    ihdr = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)
    return PNG_SIGNATURE + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"IDAT", bytes(compressed)) + _png_chunk(b"IEND", b"")


def save_camera_capture(result: dict[str, Any], device_id: str, root: str | Path | None = None) -> dict[str, Any]:
    """Validate a RGB565 action result, save its PNG, and return non-image metadata."""
    if not isinstance(result, dict) or result.get("format") != "rgb565le":
        raise ValueError("camera result is not little-endian RGB565")
    width, height = result.get("width"), result.get("height")
    if (not isinstance(width, int) or isinstance(width, bool)
            or not isinstance(height, int) or isinstance(height, bool)):
        raise ValueError("camera result has invalid dimensions")
    if not 1 <= width <= MAX_WIDTH or not 1 <= height <= MAX_HEIGHT:
        raise ValueError(f"unsupported camera size {width}x{height}")

    encoded = result.get("image_b64")
    if not isinstance(encoded, str) or len(encoded) > ((MAX_RAW_BYTES + 2) // 3) * 4:
        raise ValueError("camera result has invalid or oversized image data")
    try:
        pixels = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError("camera image is not valid base64") from exc
    if len(pixels) != width * height * 2:
        raise ValueError("RGB565 payload length doesn't match its dimensions")

    png = rgb565_png(pixels, width, height)
    directory = Path(root) if root is not None else Path.home() / ".hermes" / "gadgetpix"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    safe_device = re.sub(r"[^A-Za-z0-9_-]+", "_", str(device_id)).strip("_") or "gadget"
    target = directory / f"{stamp}_{safe_device}.png"
    with tempfile.NamedTemporaryFile(prefix=".gadgetpix-", suffix=".tmp", dir=directory, delete=False) as temp:
        temp_path = Path(temp.name)
        temp.write(png)
    try:
        temp_path.replace(target)
    finally:
        temp_path.unlink(missing_ok=True)

    return {"path": str(target), "format": "png", "width": width, "height": height, "size_bytes": len(png)}
