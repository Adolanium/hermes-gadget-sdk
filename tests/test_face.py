"""The face generator: `hermes_gadget.face` and the `hermes-gadget face` command.

The load-bearing test is the first one. The mascot profile must reproduce
`firmware/core/src/mascot_data.cpp`, byte for byte, because that file is what the
firmware ships and there is now one generator rather than two.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

pytest.importorskip("PIL", reason="the face command needs Pillow (the images extra)")

from hermes_gadget import cli, face  # noqa: E402  (after the Pillow guard)

REPO = Path(__file__).resolve().parents[1]
SHIPPED = REPO / "firmware" / "core" / "src" / "mascot_data.cpp"
MASTER = REPO / "assets" / "mascot" / "nous-girl-white-1024.png"
EXPECTED = {64: 512, 96: 1152, 144: 2592, 192: 4608}


def generate_mascot(tmp_path: Path) -> str:
    face.write_mascot(out=tmp_path / "mascot_data.cpp", preview_path=tmp_path / "preview.png")
    return (tmp_path / "mascot_data.cpp").read_text()


def arrays(text: str) -> dict[str, bytes]:
    out = {}
    for name, declared, body in re.findall(r"const uint8_t (k\w+)\[(\d+)\] = \{(.*?)\};", text, re.S):
        out[name] = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body))
        assert len(out[name]) == int(declared), f"{name} declares {declared} bytes"
    return out


def test_mascot_profile_reproduces_the_shipped_file(tmp_path):
    assert generate_mascot(tmp_path) == SHIPPED.read_text()


def test_frames_are_well_formed(tmp_path):
    text = generate_mascot(tmp_path)
    data = arrays(text)
    assert len(data) == 12, "three frames at four sizes"
    for size, nbytes in EXPECTED.items():
        for frame in ("Idle", "Blink", "Talk"):
            assert len(data[f"k{frame}{size}"]) == nbytes
    assert "const Anchors kAnchors = {" in text and "};" in text


def test_the_mascot_blinks_and_talks(tmp_path):
    """A blink frame equal to idle would be a silent failure: nothing to see."""
    data = arrays(generate_mascot(tmp_path))
    for size in EXPECTED:
        for frame in ("Blink", "Talk"):
            assert any(a != b for a, b in zip(data[f"kIdle{size}"], data[f"k{frame}{size}"])), size


def test_the_blink_is_big_enough_to_see():
    """The number the docs quote: 226 bits at 192 px, the size the screen draws."""
    master = face.load_master(MASTER)
    frames = face.mascot_frames(master)
    idle = face.to_bits(frames["idle"], 192)
    blink = face.to_bits(frames["blink"], 192)
    assert sum(bin(a ^ b).count("1") for a, b in zip(idle, blink)) == 226


def test_measured_geometry_lands_on_the_mascots_features():
    """Fractions taken from the artwork must land where its features are.

    These are the coordinates tools/gen_mascot.py used, so they pin the maths to
    the art the firmware ships. The two eyes need their own boxes: the far eye is
    a third the width of the near one and sits lower.
    """
    master = face.load_master(MASTER)
    alpha = master.getchannel("A")
    opts = face.Options(
        eye_left=(0.2197, 0.4602, 0.0482, 0.0746), eye_right=(0.3826, 0.4349, 0.1404, 0.0835),
        mouth_x=0.2562, mouth_y=0.6133, mouth_w=0.0493, mouth_h=0.0258,
        ear_cup=(0.634, 0.280), think_dot=(0.847, 0.091))
    geo = face.measure(alpha, opts)

    assert geo["mouth"] == (246, 622, 292, 648)
    for side, want in (("left", (235, 481)), ("right", (387, 456))):
        box = geo[side]
        got = ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        assert abs(got[0] - want[0]) <= 2 and abs(got[1] - want[1]) <= 2, (side, got, want)
    assert tuple(round(v) for v in geo["ear_cup"]) == (622, 300)
    assert tuple(round(v) for v in geo["think_dot"]) == (820, 110)


def test_mask_bright_keys_art_out_of_a_flat_background(tmp_path):
    Image = pytest.importorskip("PIL.Image")
    src = tmp_path / "flat.png"
    img = Image.new("RGB", (200, 200), (0, 0, 0))
    for x in range(60, 140):
        for y in range(60, 140):
            img.putpixel((x, y), (255, 255, 255))
    img.save(src)

    master = face.load_master(src, mask="bright")
    box = face.ink_box(master.getchannel("A"))
    scale = 1024 / 200  # the art is fitted to a 1024 square
    assert abs(box[0] - 60 * scale) < 12 and abs(box[2] - 139 * scale) < 12
    assert abs(box[1] - 60 * scale) < 12 and abs(box[3] - 139 * scale) < 12


def test_a_picture_without_transparency_says_so(tmp_path):
    Image = pytest.importorskip("PIL.Image")
    src = tmp_path / "opaque.png"
    Image.new("RGB", (64, 64), (10, 10, 10)).save(src)

    with pytest.raises(SystemExit) as exc:
        face.load_master(src, mask="alpha")
    assert "--mask bright" in str(exc.value)


def test_the_command_generates_the_mascot(tmp_path):
    out = tmp_path / "mascot_data.cpp"
    assert cli.main(["face", "--out", str(out), "--preview", str(tmp_path / "p.png")]) == 0
    assert out.read_text() == SHIPPED.read_text()


def test_the_command_takes_a_picture_and_writes_a_face(tmp_path):
    Image = pytest.importorskip("PIL.Image")
    src = tmp_path / "face.png"
    img = Image.new("RGB", (256, 256), (0, 0, 0))
    for x in range(60, 200):
        for y in range(40, 220):
            img.putpixel((x, y), (255, 255, 255))
    img.save(src)

    out = tmp_path / "face.cpp"
    code = cli.main(["face", str(src), "--mask", "bright", "--out", str(out),
                     "--preview", str(tmp_path / "p.png"), "--check", str(tmp_path / "c.png")])
    assert code == 0
    text = out.read_text()
    assert "const Bitmap kBitmaps[]" in text
    assert len(arrays(text)) == 12
    assert (tmp_path / "c.png").exists(), "the geometry check is how features get placed"


def test_the_command_wants_both_eyes_or_neither(tmp_path):
    Image = pytest.importorskip("PIL.Image")
    src = tmp_path / "face.png"
    Image.new("RGB", (128, 128), (0, 0, 0)).save(src)

    with pytest.raises(SystemExit) as exc:
        cli.main(["face", str(src), "--mask", "bright", "--eye-left", "0.3", "0.4", "0.1", "0.1",
                  "--out", str(tmp_path / "x.cpp")])
    assert "both" in str(exc.value)
