"""The face the device draws, and how to make one from a picture.

The firmware draws three frames, idle, blink and talk, at 64, 96, 144 and 192 px.
They ship as the Hermes Agent mascot, generated into
`firmware/core/src/mascot_data.cpp`.

Two profiles:

* **the mascot**, the default. The project's own artwork and feature positions,
  reproducing the committed file exactly. This is what `tools/gen_mascot.py`
  did before there was one generator.
* **a picture of your own.** Any drawing, where the eye and mouth positions come
  either from fractions of the drawing's outline or from measurements of it.

Both write the same kind of 1-bit bitmap, and both offer the same three pictures
for looking at before flashing anything: a contact sheet of the frames, a
geometry check drawn on the artwork, and a count of how many bits each frame
changes, which is what decides whether a blink reads on a 240x240 screen.

Pillow is needed, and lives behind the `images` extra.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from .paths import repo_root

REPO = repo_root()
MASTER = REPO / "assets" / "mascot" / "nous-girl-white-1024.png"
OUT_DEFAULT = REPO / "firmware" / "core" / "src" / "mascot_data.cpp"

SIZES = (64, 96, 144, 192)
FRAMES = ("idle", "blink", "talk")
THRESHOLD = 110  # alpha above this is ink after downsampling
MASTER_SIZE = 1024  # the master's side, in pixels: the coordinates below live in this space
BG = (10, 14, 20)  # the device's background, for the pictures we write
LID_DROP = 0.10    # where a closed lid sits inside the eye
LID_WIDTH = 0.20   # lid stroke, as a fraction of the eye's half width
LID_SAG = 0.35     # how far the lid dips in the middle

# The mascot's feature geometry, in master pixels. The eye and lid shapes are
# polygons because the artwork's eyes are, which is what makes this reproduce
# the shipped file.
MASCOT = {
    "right_eye": [(318, 418), (452, 410), (462, 470), (444, 498), (326, 496), (314, 458)],
    "right_lid": [(326, 452), (350, 466), (388, 474), (424, 468), (452, 452)],
    "left_eye": [(213, 446), (254, 444), (257, 516), (215, 518)],
    "left_lid": [(215, 484), (233, 490), (254, 484)],
    "mouth_open": (246, 622, 292, 648),
    "ear_cup": (622, 300),
    "mouth": (268, 635),
    "think_dot": (820, 110),
    "eyes_rows": (405, 520),
    "mouth_rows": (600, 665),
}


def load_master(path: Path, size: int = MASTER_SIZE, mask: str = "alpha",
                threshold: int = THRESHOLD, crop: tuple[int, int, int, int] | None = None):
    """Open a picture as a square RGBA canvas whose alpha channel is the ink.

    alpha   the picture carries its own transparency
    bright  ink is the light part: white art on a dark background
    dark    ink is the dark part: black art on a light background

    Anything from an image generator or a screenshot has no transparency, so it
    needs bright or dark. With alpha and no transparency, say so rather than
    writing a solid block.
    """
    src = Image.open(path)
    if mask == "alpha":
        art = src.convert("RGBA")
        low, high = art.getchannel("A").getextrema()
        if low == high:
            raise SystemExit(
                f"{path.name} has no transparency. Use --mask bright for light art on a dark "
                f"background, or --mask dark for dark art on a light one.")
    elif mask in ("bright", "dark"):
        lum = src.convert("L")
        keep = (lambda v: v > threshold) if mask == "bright" else (lambda v: v < threshold)
        ink = lum.point(lambda v: 255 if keep(v) else 0)
        art = Image.merge("RGBA", (Image.new("L", lum.size, 255),) * 3 + (ink,))
    else:
        raise SystemExit(f"unknown mask mode {mask!r}")

    if crop:
        art = art.crop(tuple(crop))
    if art.size == (size, size):
        return art
    scale = min(size / art.width, size / art.height)
    fitted = art.resize((max(1, round(art.width * scale)), max(1, round(art.height * scale))),
                        Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(fitted, ((size - fitted.width) // 2, (size - fitted.height) // 2), fitted)
    return canvas


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


# -- the mascot profile ---------------------------------------------------------------------------------

def mascot_frames(master):
    """The frames that ship, drawn the way the artwork's own features are."""
    alpha = master.getchannel("A")

    blink = alpha.copy()
    draw = ImageDraw.Draw(blink)
    draw.polygon(MASCOT["right_eye"], fill=255)  # skin over the open eye
    draw.polygon(MASCOT["left_eye"], fill=255)
    draw.line(MASCOT["right_lid"], fill=0, width=14, joint="curve")  # then the closed lids
    draw.line(MASCOT["left_lid"], fill=0, width=12, joint="curve")

    talk = alpha.copy()
    ImageDraw.Draw(talk).ellipse(MASCOT["mouth_open"], fill=0)  # open mouth, cut out of the ink
    return {"idle": alpha, "blink": blink, "talk": talk}


def mascot_anchors() -> dict:
    """Effect anchors for the mascot, in master pixels."""
    return {"ear_cup": MASCOT["ear_cup"], "mouth_pt": MASCOT["mouth"], "think_dot": MASCOT["think_dot"],
            "eyes_rows": MASCOT["eyes_rows"], "mouth_rows": MASCOT["mouth_rows"]}


# -- a picture of your own ------------------------------------------------------------------------------

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


def lid_cy(box) -> float:
    """Where a closed lid sits: just below the middle of that eye."""
    ry = (box[3] - box[1]) / 2
    return (box[1] + box[3]) / 2 + ry * LID_DROP


def lid_width(box) -> int:
    return max(2, round((box[2] - box[0]) / 2 * LID_WIDTH))


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


def generic_frames(alpha, geo: dict, blink_dark: bool = False):
    """The frames for a drawing of your own.

    A light eye is covered and the lid cut out of the cover; a dark eye, which is
    what shadow or a mask looks like, is darkened and the lid drawn as a light
    line instead.
    """
    blink = alpha.copy()
    draw = ImageDraw.Draw(blink)
    for side in ("left", "right"):
        x0, y0, x1, y1 = geo[side]
        cy = lid_cy(geo[side])
        sag = (y1 - y0) / 2 * LID_SAG
        if blink_dark:
            draw.ellipse((x0, y0, x1, y1), fill=0)
            draw.line((x0, cy, (x0 + x1) / 2, cy + sag, x1, cy), fill=255,
                      width=max(2, lid_width(geo[side]) // 2), joint="curve")
        else:
            draw.ellipse((x0, y0, x1, y1), fill=255)
            draw.line((x0, cy, (x0 + x1) / 2, cy + sag, x1, cy), fill=0,
                      width=lid_width(geo[side]), joint="curve")

    talk = alpha.copy()
    ImageDraw.Draw(talk).ellipse(geo["mouth"], fill=0)  # the mouth opens as a hole in the ink
    return {"idle": alpha, "blink": blink, "talk": talk}


# -- the file itself ------------------------------------------------------------------------------------

def to_bits(alpha, size: int, threshold: int = THRESHOLD) -> bytes:
    """One frame at one size: row major, MSB first, rows padded, a set bit is ink."""
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


def emit(frames, anchors: dict, source: str, threshold: int = THRESHOLD) -> str:
    """The C++ source for a face. `source` is how the header describes where it came from."""
    lines = [
        f"// Generated by hermes-gadget face from {source}. Do not edit.",
        "// Artwork: Hermes Agent mascot (MIT License, Copyright (c) 2025 Nous Research)."
        if source.endswith("nous-girl-white-1024.png")
        else "// Artwork: the drawing above; see docs/faces.md.",
        '#include "hg/mascot.hpp"',
        "",
        "namespace hg::mascot {",
        "namespace {",
        "",
    ]
    table = []
    for size in SIZES:
        for frame in FRAMES:
            name = f"k{frame.capitalize()}{size}"
            data = to_bits(frames[frame], size, threshold)
            lines.append(f"const uint8_t {name}[{len(data)}] = {{")
            for i in range(0, len(data), 24):
                lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
            lines.append("};")
            table.append((size, frame, name))
    lines += ["", "const Bitmap kBitmaps[] = {"]
    for size, frame, name in table:
        lines.append(f"    {{{size}, Frame::{frame.capitalize()}, {name}}},")
    ear, mouth, dot = anchors["ear_cup"], anchors["mouth_pt"], anchors["think_dot"]
    eyes, mouth_rows = anchors["eyes_rows"], anchors["mouth_rows"]
    a = thousandths
    lines += [
        "};",
        "",
        f"const Anchors kAnchors = {{{{{a(ear[0])}, {a(ear[1])}}}, {{{a(mouth[0])}, {a(mouth[1])}}}, "
        f"{{{a(dot[0])}, {a(dot[1])}}}, {a(eyes[0])}, {a(eyes[1])}, {a(mouth_rows[0])}, {a(mouth_rows[1])}}};",
        "",
        "}  // namespace",
        "",
        "const Anchors& anchors() { return kAnchors; }",
        "",
        "const Bitmap* pick(int max_size, Frame frame) {",
        "  const Bitmap* best = nullptr;",
        "  for (const auto& b : kBitmaps) {",
        "    if (b.frame != frame) continue;",
        "    if (b.size <= max_size && (!best || b.size > best->size)) best = &b;",
        "  }",
        "  if (best) return best;",
        "  for (const auto& b : kBitmaps) {",
        "    if (b.frame == frame && (!best || b.size < best->size)) best = &b;",
        "  }",
        "  return best;",
        "}",
        "",
        "}  // namespace hg::mascot",
        "",
    ]
    return "\n".join(lines)


# -- looking at it before flashing ----------------------------------------------------------------------

def _font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10
        return ImageFont.load_default()


def _label(draw, xy, text, fill=(210, 218, 224), size=20, anchor=None):
    draw.text(xy, text, font=_font(size), fill=fill, anchor=anchor)


def preview(frames, path: Path, threshold: int = THRESHOLD) -> None:
    """The frames as the screen shows them: a row per pose, a column per size."""
    pad, scale, label_w, head_h = 10, 3, 86, 54
    cols = [s * scale for s in SIZES]
    sheet = Image.new("RGB", (label_w + sum(c + pad for c in cols) + pad,
                              head_h + len(FRAMES) * (max(SIZES) * scale + pad) + pad), BG)
    draw = ImageDraw.Draw(sheet)
    _label(draw, (label_w, 8), "What the screen shows: each row is a pose, each column a size",
           fill=(240, 244, 248), size=22)
    x = label_w + pad
    for size, width in zip(SIZES, cols):
        _label(draw, (x + width // 2, 32), f"{size} px", size=18, anchor="ma")
        x += width + pad
    y = head_h
    for frame in FRAMES:
        row_h = max(SIZES) * scale
        _label(draw, (pad, y + row_h // 2 - 12), frame, size=22)
        x = label_w + pad
        for size in SIZES:
            ink = frames[frame].resize((size, size), Image.LANCZOS).point(
                lambda v: 255 if v > threshold else 0)
            tile = Image.new("RGB", (size, size), BG)
            tile.paste(Image.new("RGB", (size, size), (232, 238, 242)), (0, 0), ink.convert("L"))
            sheet.paste(tile.resize((size * scale, size * scale), Image.NEAREST), (x, y))
            x += size * scale + pad
        y += row_h + pad
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def overlay(master, geo: dict, path: Path, box) -> None:
    """The drawing with the feature marks on it, which is how they get placed."""
    back = Image.new("RGB", master.size, BG)
    back.paste(master, (0, 0), master)
    draw = ImageDraw.Draw(back, "RGBA")
    x0, y0, x1, y1 = box
    draw.rectangle((x0, y0, x1, y1), outline=(120, 120, 120, 160), width=2)
    for key, colour in (("eyes_rows", (255, 210, 80, 60)), ("mouth_rows", (80, 220, 255, 60))):
        draw.rectangle((x0, geo[key][0], x1, geo[key][1]), fill=colour)
    for side in ("left", "right"):
        bx0, by0, bx1, by1 = geo[side]
        draw.ellipse((bx0, by0, bx1, by1), outline=(255, 80, 80, 220), width=4)
        cy = lid_cy(geo[side])
        draw.line((bx0, cy, bx1, cy), fill=(255, 235, 60, 255), width=4)
    draw.ellipse(geo["mouth"], outline=(80, 220, 255, 255), width=4)
    for key, colour in (("ear_cup", (80, 255, 140, 255)), ("think_dot", (255, 120, 255, 255))):
        px, py = geo[key]
        draw.line((px - 26, py, px + 26, py), fill=colour, width=5)
        draw.line((px, py - 26, px, py + 26), fill=colour, width=5)

    art = back.resize((512, 512), Image.LANCZOS)
    legend = [((255, 80, 80), "red circle = an eye"),
              ((255, 235, 60), "yellow line = the lid it draws when blinking"),
              ((80, 220, 255), "cyan = the mouth, which opens when speaking"),
              ((80, 255, 140), "green cross = where the listening waves start"),
              ((255, 120, 255), "magenta cross = where the thinking dots go")]
    sheet = Image.new("RGB", (512, 512 + 30 + 24 * len(legend)), BG)
    sheet.paste(art, (0, 0))
    draw = ImageDraw.Draw(sheet)
    _label(draw, (10, 520), "Where the marks landed. Move them with the flags until they look right.",
           fill=(240, 244, 248), size=19)
    for i, (colour, text) in enumerate(legend):
        y = 548 + i * 24
        draw.rectangle((12, y + 5, 34, y + 15), fill=colour)
        _label(draw, (44, y), text, size=18)
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def report(frames, threshold: int = THRESHOLD) -> None:
    """How much each frame moves against idle, which is what decides visibility.

    The face is drawn at 192 px at most on a 240x240 screen, and the shipped
    mascot changes 226 bits there on a blink. Much less and the blink is a
    flicker nobody notices.
    """
    print("bits changed against idle (the shipped mascot changes 226 at 192 px):")
    for size in SIZES:
        idle = to_bits(frames["idle"], size, threshold)
        parts = []
        for frame in ("blink", "talk"):
            other = to_bits(frames[frame], size, threshold)
            bits = sum(bin(a ^ b).count("1") for a, b in zip(idle, other))
            parts.append(f"{frame} {bits:>5} ({bits * 100 / (size * size):.2f}%)")
        print(f"  {size:>3} px   " + "   ".join(parts))


# -- the two things the CLI calls -----------------------------------------------------------------------

def write_mascot(out: Path = OUT_DEFAULT, preview_path: Path | None = None,
                 check_path: Path | None = None, want_report: bool = False) -> Path:
    """Regenerate the shipped mascot. Byte for byte what gen_mascot.py produced."""
    master = load_master(MASTER)
    if master.size != (MASTER_SIZE, MASTER_SIZE):
        raise SystemExit(f"{MASTER} must be {MASTER_SIZE}x{MASTER_SIZE}, got {master.size}")
    alpha = master.getchannel("A")
    frames = mascot_frames(master)
    box = ink_box(alpha)
    source = str(MASTER.relative_to(REPO)) if REPO in MASTER.parents else MASTER.name

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(emit(frames, mascot_anchors(), source), encoding="utf-8")
    print(f"wrote {out}")
    preview_path = preview_path or REPO / "build" / "mascot-preview.png"
    preview(frames, preview_path)
    print(f"wrote {preview_path}")
    if want_report:
        report(frames)
    if check_path:
        overlay(master, {**mascot_anchors(), "left": (213, 444, 257, 518), "right": (314, 410, 462, 498)},
                check_path, box)
        print(f"wrote {check_path}")
    return out


def write_face(source: Path, opts: Options, out: Path = OUT_DEFAULT,
               preview_path: Path | None = None, check_path: Path | None = None,
               want_report: bool = False) -> Path:
    """Generate a face from a picture. The shipped mascot can be restored with git."""
    master = load_master(source, mask=opts.mask, threshold=opts.threshold, crop=opts.crop)
    alpha = master.getchannel("A")
    box = ink_box(alpha, opts.threshold)

    geo = measure(alpha, opts)
    frames = ({"idle": alpha, "blink": alpha, "talk": alpha} if opts.plain
              else generic_frames(alpha, geo, opts.blink == "dark"))
    label = str(source.relative_to(REPO)) if REPO in source.parents else source.name

    print(f"ink box:   x {box[0]}..{box[2]}  y {box[1]}..{box[3]}")
    print(f"eyes:      left {geo['left']}  right {geo['right']}")
    print(f"lids:      left y {lid_cy(geo['left']):.0f} w {lid_width(geo['left'])}   "
          f"right y {lid_cy(geo['right']):.0f} w {lid_width(geo['right'])}")
    print(f"mouth:     {geo['mouth']}")
    print(f"ear cup:   {tuple(round(v) for v in geo['ear_cup'])}   "
          f"think dot: {tuple(round(v) for v in geo['think_dot'])}")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(emit(frames, geo, label, opts.threshold), encoding="utf-8")
    print(f"wrote {out}")
    preview_path = preview_path or REPO / "build" / "face-preview.png"
    preview(frames, preview_path, opts.threshold)
    print(f"wrote {preview_path}")
    if want_report:
        report(frames, opts.threshold)
    check_path = check_path or REPO / "build" / "face-check.png"
    overlay(master, geo, check_path, box)
    print(f"wrote {check_path}  <- look here first")
    return out
