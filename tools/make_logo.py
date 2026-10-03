"""Draw the project logo from the Hermes Agent mascot artwork.

    python tools/make_logo.py            # writes docs/images/logo.png

Needs Pillow.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[1]
MASCOT = REPO / "assets" / "mascot" / "nous-girl-white-1024.png"
AMBER = (242, 179, 61)


def vertical_gradient(size, top, bottom) -> Image.Image:
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        grad.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return grad.resize((w, h))


def rounded_mask(size, radius) -> Image.Image:
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return mask


def render_logo(path: Path, size: int = 512) -> None:
    tile = vertical_gradient((size, size), (28, 36, 50), (9, 12, 18)).convert("RGBA")
    d = ImageDraw.Draw(tile)
    # Soft amber glow behind her headphones.
    glow = Image.new("L", (size, size), 0)
    ImageDraw.Draw(glow).ellipse((size * 0.38, size * 0.08, size * 1.05, size * 0.75), fill=90)
    tile.paste(Image.new("RGBA", (size, size), AMBER + (255,)), (0, 0), glow.filter(ImageFilter.GaussianBlur(size // 8)))
    art = Image.open(MASCOT).convert("RGBA")
    a = int(size * 0.86)
    art = art.resize((a, a), Image.LANCZOS)
    ax, ay = (size - a) // 2 - int(size * 0.02), size - a + int(size * 0.06)
    tile.alpha_composite(art, (ax, ay))
    # "Listening" arcs from the ear cup, the device's signature effect.
    cx, cy = ax + a * 0.622, ay + a * 0.300
    for i, r in enumerate((0.13, 0.19, 0.25)):
        rr = a * r
        d.arc((cx - rr, cy - rr, cx + rr, cy + rr), -50, 50, fill=AMBER + (255 - 60 * i,), width=max(3, size // 64))
    mask = rounded_mask((size, size), size // 5)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(tile, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle((1, 1, size - 2, size - 2), size // 5, outline=(255, 255, 255, 38), width=2)
    out.save(path)


if __name__ == "__main__":
    render_logo(REPO / "docs" / "images" / "logo.png")
