"""Faces for the device: its three frames at four sizes, as the `face` message carries them, or a
still colour picture, as `face.start` does.

`hermes-gadget face` makes a face from a drawing and writes it into the firmware or, as
`gadget-face.bin`, into a Hermes profile's folder. The plugin sends a profile's face to devices
that draw faces: that file, or else the profile's Hermes avatar made into a still face, in colour
where the device draws colour faces. Pillow is needed to make a face from a picture, not to send a
face file.
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
COLOUR_AVATAR = b"hermes-gadget colour avatar 1\n"  # followed by the size, as two little-endian bytes
COLOUR_FRAMES = b"hermes-gadget colour avatar frames 1\n"  # then the size, and each picture's length and bytes
PICTURE_TYPES = ("png", "jpg", "webp")
SAME = 16  # a frame's pixel within this many levels (of 255) of the avatar's is unchanged: lossy WebP's noise


def digest(data: bytes) -> str:
    """The `hash` that names a face on the wire."""
    return hashlib.sha256(data).hexdigest()[:16]


@dataclass(frozen=True)
class ProfileFace:
    """A profile's face, named before it is made: `hash` comes from the source file, read at once,
    and `load()` makes the face's bytes, which for an avatar means converting it. `valid()` says
    whether the face may still be shown, as when the profile it is from still approves the device."""

    hash: str
    load: Callable[[], "bytes | ColourFace"]
    valid: Callable[[], bool] = lambda: True

    @classmethod
    def of(cls, data: bytes) -> "ProfileFace":
        """A face given by its bytes, as a face file gives it."""
        return cls(digest(FACE_FILE_KIND + data), lambda: data)


@dataclass(frozen=True)
class ColourFace:
    """A colour face (docs/protocol.md#faces): px*px little-endian RGB565, row-major, where 0x0000 is
    transparent; the effect anchors in thousandths, in the firmware's order; and the rows its blink and
    talk frames change, as (first row, rows, their pixels), or None where the face has no such frame."""

    px: int
    rgb565: bytes
    anchors: tuple[int, ...]
    blink: tuple[int, int, bytes] | None = None
    talk: tuple[int, int, bytes] | None = None


def anchor_values(anchors: dict) -> list[int]:
    """measure()'s anchors as the thousandths the device takes, held to its 0..1000."""
    return [min(1000, max(0, thousandths(v))) for name in ANCHOR_FIELDS for v in anchors[name]]


def pack(frames, anchors: dict, threshold: int = THRESHOLD) -> bytes:
    """A face's bytes (docs/protocol.md#faces): every size, smallest first, each as idle, blink and
    talk; then the effect anchors as little-endian int16 thousandths, in the firmware's order, held
    to the 0..1000 the device accepts."""
    bits = b"".join(to_bits(frames[frame], size, threshold) for size in SIZES for frame in FRAMES)
    values = anchor_values(anchors)
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


def colour_from_picture(path: Path | io.BytesIO, px: int) -> ColourFace:
    """A still colour face, px wide, from a picture such as a Hermes avatar.

    An opaque picture is cropped to the square. One with transparency is fitted whole, so it keeps
    its shape: its pixels under half opaque are transparent, the rest are laid on black, and black
    itself becomes the nearest colour that is not transparent. The waves and dots go where they would
    on a one-colour face made from the same outline.
    """
    from PIL import Image, ImageOps

    from .imaging import rgb565_bytes

    with Image.open(path) as src:
        src = ImageOps.exif_transpose(src).convert("RGBA")
    if src.getchannel("A").getextrema()[0] == 255:
        art = ImageOps.fit(src, (px, px), Image.Resampling.LANCZOS)
    else:
        whole = ImageOps.contain(src, (px, px), Image.Resampling.LANCZOS)
        art = Image.new("RGBA", (px, px))
        art.paste(whole, ((px - whole.width) // 2, (px - whole.height) // 2))
    alpha = art.getchannel("A")
    rgb = Image.new("RGB", art.size)
    rgb.paste(art, mask=alpha)
    data = bytearray(rgb565_bytes(rgb))
    for i, a in enumerate(alpha.tobytes()):
        if a < 128:
            data[2 * i: 2 * i + 2] = b"\0\0"
        elif not data[2 * i] and not data[2 * i + 1]:
            data[2 * i] = 0x20  # black, one green step up: 0x0000 means transparent
    shape = alpha.point(lambda v: 255 if v >= 128 else 0).resize((MASTER_SIZE, MASTER_SIZE))
    return ColourFace(px, bytes(data), tuple(anchor_values(measure(shape, Options()))))


def _differs(a: bytes, b: bytes) -> bool:
    """Whether two rows of RGB565 differ in transparency, or by more than SAME in a colour."""
    if a == b:
        return False
    for (p,), (q,) in zip(struct.iter_unpack("<H", a), struct.iter_unpack("<H", b)):
        if (not p) != (not q) or max(abs((p >> 11) - (q >> 11)) * 8, abs((p >> 5 & 63) - (q >> 5 & 63)) * 4,
                                     abs((p & 31) - (q & 31)) * 8) > SAME:
            return True
    return False


_refused: set[tuple] = set()  # what the log already said: a frame's path, picture hash and px


def _refuse(key: tuple, message: str, *args) -> None:
    """Log `message` once for `key`."""
    if key not in _refused:
        _refused.add(key)
        logger.warning(message, *args)


def moving(still: ColourFace, avatar: str, frames: dict[str, tuple[Path, bytes]]) -> ColourFace:
    """`still` with the blink and talk frames of `frames` (frame: (path, picture)), each as only the
    rows where it differs from the avatar. The bands share half the face's rows. A frame that is no
    picture, or needs more rows than that, leaves the whole face still, and the log says why, once for
    each file."""
    px, row = still.px, 2 * still.px
    room, bands = px // 2, {}
    for frame, feature in (("blink", "eyes"), ("talk", "mouth")):
        if frame not in frames:
            continue
        path, picture = frames[frame]
        try:
            drawn = colour_from_picture(io.BytesIO(picture), px).rgb565
        except Exception as exc:  # an optional file: the avatar still shows
            _refuse((path, digest(picture), px), "%s is not a picture the plugin can read (%s); the face stays still",
                    path, exc)
            return still
        changed = [y for y in range(px) if _differs(drawn[y * row:(y + 1) * row], still.rgb565[y * row:(y + 1) * row])]
        if not changed:
            continue
        y0, y1 = changed[0], changed[-1] + 1
        if y1 - y0 > room:
            _refuse((path, digest(picture), px), "%s differs from %s in %d of %d rows, more than the %d the device has "
                    "room for; draw it from the same picture with only the %s changed", path, avatar, y1 - y0, px,
                    room, feature)
            return still
        bands[frame] = (y0, y1 - y0, drawn[y0 * row:y1 * row])
    rows = sum(n for _, n, _ in bands.values())
    if rows > room:
        paths = [frames[frame][0] for frame in bands]
        _refuse((*paths, *(digest(frames[frame][1]) for frame in bands), px),
                "%s and %s differ from %s in %d rows together, more than the %d the device has room for; the face "
                "stays still", *paths, avatar, rows, room)
        return still
    return ColourFace(px, still.rgb565, still.anchors, **bands)


def _pictures(home: Path, name: str) -> list[Path]:
    return [home / "assets" / f"{name}.{ext}" for ext in PICTURE_TYPES]


def sources(home: Path, convert: bool = True) -> list[Path]:
    """Where a profile's face may come from, in the order for_profile tries them: its face file, then,
    when `convert`, its avatar (`assets/avatar.*`, as Hermes keeps it) and the avatar's blink and talk
    pictures (`assets/avatar-blink.*`, `assets/avatar-talk.*`)."""
    names = ("avatar", "avatar-blink", "avatar-talk") if convert else ()
    return [home / FACE_FILE, *(path for name in names for path in _pictures(home, name))]


def stamp(home: Path, convert: bool = True) -> tuple:
    """The path, size and modification time of each of a profile's face sources that exists: it
    changes when the profile's face may have. This reads no file."""
    found = []
    for path in sources(home, convert):
        try:
            st = path.stat()
        except OSError:
            continue
        found.append((path, st.st_size, st.st_mtime_ns))
    return tuple(found)


def for_profile(home: Path, convert: bool = True, colour_px: int = 0, frames: bool = False) -> ProfileFace | None:
    """The face a Hermes profile shows: its face file, else its avatar when `convert`, in colour at
    `colour_px` when that is set, with its blink and talk pictures when `frames`, else None for the
    mascot. This reads the files the face is made from; converting an avatar waits for `load()`."""
    path = home / FACE_FILE
    if path.is_file():
        data = path.read_bytes()
        if len(data) == FACE_BYTES:
            return ProfileFace.of(data)
        logger.warning("%s is not a face file (%d bytes, not %d); make it with hermes-gadget face --out",
                       path, len(data), FACE_BYTES)
    avatar = next((p for p in _pictures(home, "avatar") if p.is_file()), None) if convert else None
    if avatar is None:
        return None
    source = avatar.read_bytes()
    if not colour_px:
        return ProfileFace(digest(AVATAR_DRAWING + source), lambda: from_picture(io.BytesIO(source)))
    still = lambda: colour_from_picture(io.BytesIO(source), colour_px)  # noqa: E731
    size = colour_px.to_bytes(2, "little")
    plain = ProfileFace(digest(COLOUR_AVATAR + size + source), still)
    found = {}
    for frame in ("blink", "talk") if frames else ():
        picture = next((p for p in _pictures(home, f"avatar-{frame}") if p.is_file()), None)
        if not picture:
            continue
        try:
            found[frame] = (picture, picture.read_bytes())
        except OSError as exc:  # an optional file: the avatar still shows
            _refuse((picture, str(exc)), "%s cannot be read (%s); the face stays still", picture, exc)
            return plain
    if not found:
        return plain
    # Each frame is there (1, then its length and bytes) or not (0), so an empty file is not a missing one.
    named = len(source).to_bytes(4, "little") + source + b"".join(
        b"\1" + len(found[frame][1]).to_bytes(4, "little") + found[frame][1] if frame in found else b"\0"
        for frame in ("blink", "talk"))
    return ProfileFace(digest(COLOUR_FRAMES + size + named), lambda: moving(still(), avatar.name, found))
