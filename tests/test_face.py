"""The face generator: `hermes_gadget.face` and the `hermes-gadget face` command.

The mascot command must reproduce `firmware/core/src/mascot_data.cpp`, byte for
byte, because that file is what the firmware ships.
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
    """The numbers the docs quote: 226 bits on a blink and 16 on talk at 192 px, the size the screen draws."""
    master = face.load_master(MASTER)
    frames = face.mascot_frames(master)
    idle = face.to_bits(frames["idle"], 192)
    changed = {frame: sum(bin(a ^ b).count("1") for a, b in zip(idle, face.to_bits(frames[frame], 192)))
               for frame in ("blink", "talk")}
    assert changed == {"blink": 226, "talk": 16}


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
    # The same bytes on every OS: forward slashes in the header and LF line endings, even on Windows.
    raw = out.read_bytes()
    assert b"\r" not in raw and b"from assets/mascot/nous-girl-white-1024.png" in raw


def test_the_mascot_has_a_geometry_check_too(tmp_path):
    check = tmp_path / "check.png"
    assert cli.main(["face", "--out", str(tmp_path / "m.cpp"), "--preview", str(tmp_path / "p.png"),
                     "--check", str(check)]) == 0
    assert check.exists()


def test_outside_a_checkout_the_command_says_where_to_run_it(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(face, "MASTER", tmp_path / "missing.png")
    assert cli.main(["face", "--out", str(tmp_path / "x.cpp")]) == 1
    assert "runs from a checkout of the SDK" in capsys.readouterr().err
    assert not (tmp_path / "x.cpp").exists()


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


# -- placing the features by hand ------------------------------------------------------------------------

def parse_flags(line: str) -> dict:
    """Split a flag line back into its parts, so a test can read the numbers."""
    out, tokens, i = {}, line.split(), 0
    scalar = ("--mask", "--mouth-x", "--mouth-y", "--mouth-w", "--mouth-h")
    while i < len(tokens):
        key = tokens[i]
        if key in scalar:
            out[key] = tokens[i + 1]
            i += 2
        elif key.startswith("--"):
            values, j = [], i + 1
            while j < len(tokens) and not tokens[j].startswith("--"):
                values.append(tokens[j])
                j += 1
            out[key] = values
            i = j
        else:
            i += 1
    return out


def mascot_picker():
    """A picker over the mascot's own measurement, which the clicks are checked against."""
    master = face.load_master(MASTER)
    alpha = master.getchannel("A")
    opts = face.Options(
        eye_left=(0.2197, 0.4602, 0.0482, 0.0746), eye_right=(0.3826, 0.4349, 0.1404, 0.0835),
        mouth_x=0.2562, mouth_y=0.6133, mouth_w=0.0493, mouth_h=0.0258)
    geo = face.measure(alpha, opts)
    return face.Picker(face.ink_box(alpha), geo), geo


def test_clicking_the_mascots_features_gives_back_its_own_numbers():
    """The round trip that makes the picker trustworthy: click her art, get her flags.

    These are the coordinates the shipped frames were generated from, so a picker
    that reproduces them can be trusted on a picture nobody has measured.
    """
    picker, geo = mascot_picker()
    for name in ("left", "right"):
        x0, y0, x1, y1 = geo[name]
        picker.click((x0 + x1) / 2, (y0 + y1) / 2)
    x0, y0, x1, y1 = geo["mouth"]
    picker.click((x0 + x1) / 2, (y0 + y1) / 2)

    flags = parse_flags(picker.flags())
    assert flags["--mask"] == "alpha"
    assert abs(float(flags["--eye-left"][0]) - 0.2197) < 0.003
    assert abs(float(flags["--eye-left"][1]) - 0.4602) < 0.003
    assert abs(float(flags["--eye-left"][2]) - 0.0482) < 0.003
    assert abs(float(flags["--eye-right"][0]) - 0.3826) < 0.003
    assert abs(float(flags["--eye-right"][2]) - 0.1404) < 0.003
    assert abs(float(flags["--mouth-x"]) - 0.2562) < 0.003
    assert abs(float(flags["--mouth-y"]) - 0.6133) < 0.003


def test_a_click_moves_the_mark_and_keeps_its_size():
    """The click gives a position; the size stays what the measurement found."""
    picker, geo = mascot_picker()
    picker.click(300, 300)   # the near eye, deliberately nowhere near the art
    picker.click(700, 300)
    picker.click(400, 700)

    marks = picker.marks()
    for name, click in (("left", 300), ("right", 700)):
        x0, y0, x1, y1 = marks[name]
        assert abs((x0 + x1) / 2 - click) <= 1, name
        assert (x1 - x0, y1 - y0) == (geo[name][2] - geo[name][0],
                                      geo[name][3] - geo[name][1]), name
    x0, y0, x1, y1 = marks["mouth"]
    assert abs((x0 + x1) / 2 - 400) <= 1 and abs((y0 + y1) / 2 - 700) <= 1


def test_the_picker_waits_for_all_three_features():
    picker, geo = mascot_picker()
    assert picker.flags() is None
    picker.click(200, 400)
    picker.click(400, 400)
    assert picker.flags() is None, "the mouth is still missing"
    picker.click(300, 600)
    assert picker.flags() is not None
    assert "eye-left" in picker.prompt().lower() or picker.next_step() in ("ear_cup", "think_dot")


def test_return_skips_only_the_optional_anchors():
    picker, geo = mascot_picker()
    assert picker.skip() is False, "a required step cannot be skipped"
    picker.click(200, 400)   # near eye
    picker.click(400, 400)   # far eye
    picker.click(300, 600)   # mouth
    assert picker.next_step() == "ear_cup"
    assert picker.skip() is True, "the ear cup can be left to the geometry"
    assert picker.skip() is True, "and so can the think dot"
    assert picker.next_step() is None
    flags = parse_flags(picker.flags())
    assert "--ear-cup" not in flags and "--think-dot" not in flags


def test_a_picked_anchor_overrides_the_default():
    picker, geo = mascot_picker()
    picker.click(200, 400)
    picker.click(400, 400)
    picker.click(300, 600)
    picker.click(700, 200)   # where the waves should start on this art
    flags = parse_flags(picker.flags())
    assert len(flags["--ear-cup"]) == 2
    assert "--think-dot" not in flags, "skipping one anchor does not require the other"


def test_pick_without_a_picture_is_refused(capsys):
    assert cli.main(["face", "--pick"]) == 1
    assert "--pick needs a picture" in capsys.readouterr().err


def test_the_picker_says_what_it_needs_when_there_is_no_tk(tmp_path, monkeypatch):
    """The venv on the machine this was written on has no _tkinter; the message must say so."""
    import sys
    Image = pytest.importorskip("PIL.Image")
    src = tmp_path / "face.png"
    Image.new("RGB", (128, 128), (0, 0, 0)).save(src)
    monkeypatch.setitem(sys.modules, "tkinter", None)  # None in sys.modules halts the import

    with pytest.raises(SystemExit) as exc:
        face.pick_features(src, face.Options(mask="bright"))
    assert "Tk" in str(exc.value) and "brew install python-tk" in str(exc.value)
