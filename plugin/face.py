"""Faces for the device: its three frames at four sizes, as the `face` message carries them.

`hermes-gadget face` makes a face from a drawing and writes it into the firmware or, as
`gadget-face.bin`, into a Hermes profile's folder. The plugin sends a profile's face to devices
that draw faces: that file, or else the profile's Hermes avatar made into a still face. Pillow is
needed to make a face from a picture, not to send a face file.
"""

from __future__ import annotations

import hashlib
import io
import logging
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

logger = logging.getLogger(__name__)

SIZES = (64, 96, 144, 192)
FRAMES = ("idle", "blink", "talk")
THRESHOLD = 110  # alpha above this is ink after downsampling
MASTER_SIZE = 1024  # a drawing's side, in pixels, before it is cut down to SIZES
ONE_COLOUR = 16  # a picture's solid pixels whose brightness spans at most this are one colour
SOLID = 250  # alpha from which a source pixel counts as solid, not an edge blended with transparency


def ink_box(alpha, threshold: int = THRESHOLD) -> tuple[int, int, int, int]:
    """Where the drawing is, so fractions can be taken of it."""
    box = alpha.point(lambda v: 255 if v > threshold else 0).getbbox()
    return box or (0, 0, alpha.width, alpha.height)


def thousandths(master_px: int) -> int:
    """Master pixels to the thousandths of the drawn size the firmware expects.

    The renderer multiplies by the drawn size and divides by 1000, so a master
    coordinate has to be scaled: at 192 px, skipping this puts every effect
    about 3 px out.
    """
    return round(master_px * 1000 / MASTER_SIZE)


@dataclass
class Options:
    """How to read a drawing and where its features are, as fractions of its outline."""

    mask: str = "alpha"
    threshold: int = THRESHOLD
    crop: tuple[int, int, int, int] | None = None
    plain: bool = False
    blink: str = "light"
    face_x: float = 0.50
    eye_line: float = 0.40
    eye_span: float = 0.22
    eye_size: float = 0.05
    eye_ratio: float = 0.60
    eye_left: tuple[float, float, float, float] | None = None
    eye_right: tuple[float, float, float, float] | None = None
    eye_grow: float = 1.0
    mouth_grow: float = 1.0
    mouth_x: float | None = None
    mouth_y: float = 0.62
    mouth_w: float = 0.10
    mouth_h: float = 0.045
    ear_cup: tuple[float, float] = (0.62, 0.30)
    think_dot: tuple[float, float] = (0.82, 0.11)


def _eye_box(spec, box) -> tuple[int, int, int, int]:
    """A feature box from fractions of the outline: centre x, centre y, width, height."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    cx, cy = x0 + w * spec[0], y0 + h * spec[1]
    rx, ry = w * spec[2] / 2, h * spec[3] / 2
    return (round(cx - rx), round(cy - ry), round(cx + rx), round(cy + ry))


def _grow(box, factor: float):
    """Scale a box about its own centre."""
    if factor == 1.0:
        return box
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    rx, ry = (box[2] - box[0]) / 2 * factor, (box[3] - box[1]) / 2 * factor
    return (round(cx - rx), round(cy - ry), round(cx + rx), round(cy + ry))


def measure(alpha, opts: Options) -> dict:
    """Where the features are, in master pixels, from fractions of the outline."""
    x0, y0, x1, y1 = ink_box(alpha, opts.threshold)
    w, h = x1 - x0, y1 - y0
    cx = x0 + w * opts.face_x
    eye_y = y0 + h * opts.eye_line
    eye_dx = w * opts.eye_span / 2
    eye_r = w * opts.eye_size
    eye_ry = eye_r * opts.eye_ratio

    if opts.eye_left or opts.eye_right:
        if not (opts.eye_left and opts.eye_right):
            raise SystemExit("give both --eye-left and --eye-right, or neither")
        left = _eye_box(tuple(opts.eye_left), (x0, y0, x1, y1))
        right = _eye_box(tuple(opts.eye_right), (x0, y0, x1, y1))
    else:
        left = (round(cx - eye_dx - eye_r), round(eye_y - eye_ry),
                round(cx - eye_dx + eye_r), round(eye_y + eye_ry))
        right = (round(cx + eye_dx - eye_r), round(eye_y - eye_ry),
                 round(cx + eye_dx + eye_r), round(eye_y + eye_ry))
    left, right = _grow(left, opts.eye_grow), _grow(right, opts.eye_grow)

    mouth_x = opts.face_x if opts.mouth_x is None else opts.mouth_x
    mouth_cx, mouth_cy = x0 + w * mouth_x, y0 + h * opts.mouth_y
    mouth = _grow((round(mouth_cx - w * opts.mouth_w / 2), round(mouth_cy - h * opts.mouth_h / 2),
                   round(mouth_cx + w * opts.mouth_w / 2), round(mouth_cy + h * opts.mouth_h / 2)),
                  opts.mouth_grow)
    return {
        "left": left,
        "right": right,
        "mouth": mouth,
        "ear_cup": (x0 + w * opts.ear_cup[0], y0 + h * opts.ear_cup[1]),
        "mouth_pt": (mouth_cx, mouth_cy),
        "think_dot": (x0 + w * opts.think_dot[0], y0 + h * opts.think_dot[1]),
        "eyes_rows": (min(left[1], right[1]), max(left[3], right[3])),
        "mouth_rows": (round(mouth_cy - h * opts.mouth_h / 2 * 2.2),
                       round(mouth_cy + h * opts.mouth_h / 2 * 2.2)),
    }


def to_bits(alpha, size: int, threshold: int = THRESHOLD) -> bytes:
    """One frame at one size: row major, MSB first, rows padded, a set bit is ink."""
    from PIL import Image

    small = alpha.resize((size, size), Image.LANCZOS).point(lambda v: 255 if v > threshold else 0)
    px = small.load()
    out = bytearray()
    for y in range(size):
        byte, bits = 0, 0
        for x in range(size):
            byte = (byte << 1) | (1 if px[x, y] else 0)
            bits += 1
            if bits == 8:
                out.append(byte)
                byte, bits = 0, 0
        if bits:
            out.append(byte << (8 - bits))
    return bytes(out)


ANCHOR_FIELDS = ("ear_cup", "mouth_pt", "think_dot", "eyes_rows", "mouth_rows")  # two values each
FACE_BYTES = sum(3 * ((size + 7) // 8) * size for size in SIZES) + 2 * 2 * len(ANCHOR_FIELDS)
FACE_FILE = "gadget-face.bin"
# What a face's hash is taken of starts with its kind, so a face file and an avatar never share a
# hash. Change AVATAR_DRAWING when from_picture draws differently, so devices that hold the old
# drawing get the new one.
FACE_FILE_KIND = b"hermes-gadget face file\n"
AVATAR_DRAWING = b"hermes-gadget avatar 2\n"


def digest(data: bytes) -> str:
    """The `hash` that names a face on the wire."""
    return hashlib.sha256(data).hexdigest()[:16]


@dataclass(frozen=True)
class ProfileFace:
    """A profile's face, named before it is made: `hash` comes from the source file, read at once,
    and `load()` makes the face's bytes, which for an avatar means converting it. `valid()` says
    whether the face may still be shown, as when the profile it is from still approves the device."""

    hash: str
    load: Callable[[], bytes]
    valid: Callable[[], bool] = lambda: True

    @classmethod
    def of(cls, data: bytes) -> "ProfileFace":
        """A face given by its bytes, as a face file gives it."""
        return cls(digest(FACE_FILE_KIND + data), lambda: data)


def pack(frames, anchors: dict, threshold: int = THRESHOLD) -> bytes:
    """A face's bytes (docs/protocol.md#faces): every size, smallest first, each as idle, blink and
    talk; then the effect anchors as little-endian int16 thousandths, in the firmware's order, held
    to the 0..1000 the device accepts."""
    bits = b"".join(to_bits(frames[frame], size, threshold) for size in SIZES for frame in FRAMES)
    values = [min(1000, max(0, thousandths(v))) for name in ANCHOR_FIELDS for v in anchors[name]]
    return bits + struct.pack(f"<{len(values)}h", *values)


def from_picture(path: Path | io.BytesIO) -> bytes:
    """A still face from a picture not drawn for the device, such as a Hermes avatar.

    The screen draws light ink on dark, so a picture on a light background gives its dark parts
    as ink, and one on a dark background its light parts. The cut is the picture's own average
    brightness, so a dim photo keeps its features. A drawing in one colour on transparency has no
    parts on either side of that cut, so its visible pixels are the shape instead; whether it is one
    colour is judged on the picture as given, before resizing blends its edges. A still face has no
    eyes or mouth placed by hand, so it neither blinks nor talks: a face file does.
    """
    from PIL import Image, ImageOps, ImageStat

    with Image.open(path) as src:
        src = ImageOps.exif_transpose(src).convert("RGBA")
        art = ImageOps.fit(src, (MASTER_SIZE, MASTER_SIZE), Image.Resampling.LANCZOS)
    solid = src.getchannel("A").point(lambda v: 255 if v >= SOLID else 0)
    opaque = art.getchannel("A").point(lambda v: 255 if v > THRESHOLD else 0)
    lum = Image.composite(art.convert("L"), Image.new("L", art.size, 0), opaque)
    cut = ImageStat.Stat(lum, opaque).mean[0] if opaque.getbbox() else THRESHOLD
    edge = MASTER_SIZE // 25
    frame = Image.new("L", art.size, 255)
    frame.paste(0, (edge, edge, MASTER_SIZE - edge, MASTER_SIZE - edge))
    light_background = ImageStat.Stat(lum, frame).mean[0] > cut
    ink = lum.point(lambda v: 255 if (v < cut if light_background else v > cut) else 0)
    ink = Image.composite(ink, Image.new("L", art.size, 0), opaque)
    if opaque.getextrema()[0] == 0 and solid.getbbox():  # some of it is transparent
        low, high = ImageStat.Stat(src.convert("L"), solid).extrema[0]
        if high - low <= ONE_COLOUR:
            ink = opaque  # a drawing in one colour: the cut finds nothing but its blended edge
    return pack(dict.fromkeys(FRAMES, ink), measure(ink, Options()))


def for_profile(home: Path, convert: bool = True) -> ProfileFace | None:
    """The face a Hermes profile shows: its face file, else its avatar (`assets/avatar.*`, as
    Hermes keeps it) when `convert`, else None for the mascot. This reads one file; converting an
    avatar waits for `load()`."""
    path = home / FACE_FILE
    if path.is_file():
        data = path.read_bytes()
        if len(data) == FACE_BYTES:
            return ProfileFace.of(data)
        logger.warning("%s is not a face file (%d bytes, not %d); make it with hermes-gadget face --out",
                       path, len(data), FACE_BYTES)
    if convert:
        for ext in ("png", "jpg", "webp"):
            avatar = home / "assets" / f"avatar.{ext}"
            if avatar.is_file():
                source = avatar.read_bytes()
                return ProfileFace(digest(AVATAR_DRAWING + source), lambda: from_picture(io.BytesIO(source)))
    return None
