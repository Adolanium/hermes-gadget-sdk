"""Convert images to the raw RGB565 the device blits directly.

Doing the decode and scaling on the host means a gadget needs no JPEG/PNG
decoder and no scratch memory beyond its framebuffer. Requires Pillow; without
it image delivery degrades to a text notice.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class DeviceImage:
    width: int
    height: int
    rgb565: bytes  # little-endian, row-major


def pillow_available() -> bool:
    try:
        import PIL.Image  # noqa: F401
    except ImportError:
        return False
    return True


def to_rgb565(source, max_width: int, max_height: int) -> DeviceImage:
    """``source`` is a path or a file-like object."""
    from PIL import Image, ImageOps

    with Image.open(source) as img:
        img = ImageOps.exif_transpose(img)
        if img.mode in ("RGBA", "LA", "P"):
            img = img.convert("RGBA")
            bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
            bg.alpha_composite(img)
            img = bg
        img = img.convert("RGB")
        img.thumbnail((max_width, max_height), Image.Resampling.LANCZOS)
        w, h = img.size
        raw = img.tobytes()
    out = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        struct.pack_into("<H", out, 2 * i, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return DeviceImage(w, h, bytes(out))
