import base64
import binascii
import re
import struct
import zlib
from pathlib import Path

import pytest

from hermes_gadget_plugin.camera_capture import MAX_RAW_BYTES, rgb565_png, save_camera_capture
from hermes_gadget_plugin.hub import MAX_FRAME_BYTES


def _png_pixels(png: bytes) -> tuple[int, int, bytes]:
    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    width = height = 0
    compressed = bytearray()
    offset = 8
    while offset < len(png):
        size = struct.unpack_from(">I", png, offset)[0]
        kind = png[offset + 4:offset + 8]
        data = png[offset + 8:offset + 8 + size]
        expected_crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
        assert binascii.crc32(data, binascii.crc32(kind)) & 0xFFFFFFFF == expected_crc
        if kind == b"IHDR":
            width, height = struct.unpack_from(">II", data)
        elif kind == b"IDAT":
            compressed.extend(data)
        offset += size + 12
        if kind == b"IEND":
            break
    return width, height, zlib.decompress(compressed)


def test_rgb565_png_encodes_little_endian_pixels_as_png_rgb():
    pixels = bytes.fromhex("00f8 e007 1f00 ffff")

    png = rgb565_png(pixels, 2, 2)

    width, height, decoded = _png_pixels(png)
    assert (width, height) == (2, 2)
    assert decoded == bytes((
        0, 255, 0, 0, 0, 255, 0,
        0, 0, 0, 255, 255, 255, 255,
    ))


def test_camera_capture_saves_timestamped_png_under_gateway_home(monkeypatch, tmp_path):
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))  # HOME alone misses Windows
    pixels = bytes.fromhex("00f8 e007 1f00 ffff")
    result = {
        "format": "rgb565le",
        "width": 2,
        "height": 2,
        "image_b64": base64.b64encode(pixels).decode("ascii"),
    }

    saved = save_camera_capture(result, "hg-test")

    path = Path(saved["path"])
    assert path.parent == tmp_path / ".hermes" / "gadgetpix"
    assert re.fullmatch(r"\d{8}T\d{6}\.\d{6}Z_hg-test\.png", path.name)
    assert path.is_file()
    assert saved["format"] == "png"
    assert saved["size_bytes"] == path.stat().st_size
    assert "image_b64" not in saved
    assert _png_pixels(path.read_bytes()) == (2, 2, bytes((
        0, 255, 0, 0, 0, 255, 0,
        0, 0, 0, 255, 255, 255, 255,
    )))


def test_camera_capture_rejects_bad_payload_and_oversized_dimensions(tmp_path):
    invalid_base64 = {"format": "rgb565le", "width": 1, "height": 1, "image_b64": "%%%"}
    with pytest.raises(ValueError, match="base64"):
        save_camera_capture(invalid_base64, "hg-test", tmp_path)

    oversized = {
        "format": "rgb565le",
        "width": 641,
        "height": 1,
        "image_b64": base64.b64encode(bytes(641 * 2)).decode("ascii"),
    }
    with pytest.raises(ValueError, match="unsupported camera size"):
        save_camera_capture(oversized, "hg-test", tmp_path)


def test_camera_action_returns_saved_png_metadata_not_base64(monkeypatch):
    import json
    from types import SimpleNamespace

    from hermes_gadget_plugin import camera_capture, tools

    result = {"format": "rgb565le", "width": 1, "height": 1, "image_b64": "AA=="}
    saved = {"path": "/home/test/.hermes/gadgetpix/photo.png", "format": "png", "width": 1,
             "height": 1, "size_bytes": 68}
    session = SimpleNamespace(name="7B", device_id="hg-test", paired=True,
                              invoke_action=lambda *args, **kwargs: object())
    adapter = object()
    monkeypatch.setattr(tools, "_adapter", lambda: adapter)
    monkeypatch.setattr(tools, "_resolve", lambda _adapter, _device: (session, None))
    monkeypatch.setattr(tools, "_usable", lambda _session: None)
    monkeypatch.setattr(tools, "_run", lambda _adapter, _coro: result)
    monkeypatch.setattr(camera_capture, "save_camera_capture", lambda value, device_id: saved)

    response = json.loads(tools.gadget_action({"action": "camera.capture"}))

    assert response["success"] is True
    assert response["path"] == saved["path"]
    assert response["format"] == "png"
    assert response["device"] == "7B"
    assert "result" not in response
    assert "image_b64" not in response


def test_camera_action_result_fits_the_existing_websocket_frame_limit():
    import json

    image = {
        "format": "rgb565le",
        "width": 640,
        "height": 512,
        "image_b64": base64.b64encode(bytes(MAX_RAW_BYTES)).decode("ascii"),
    }
    frame = json.dumps({"type": "action.result", "id": "0123456789ab", "ok": True, "result": image},
                       separators=(",", ":")).encode("utf-8")

    assert len(frame) < MAX_FRAME_BYTES


def test_camera_capture_keeps_only_the_newest_captures(monkeypatch, tmp_path):
    from hermes_gadget_plugin import camera_capture

    monkeypatch.setattr(camera_capture, "KEEP_CAPTURES", 3)
    unrelated = tmp_path / "holiday.png"
    unrelated.write_bytes(b"not ours")
    result = {"format": "rgb565le", "width": 1, "height": 1, "image_b64": "AAA="}
    saved = [Path(camera_capture.save_camera_capture(result, "hg-test", tmp_path)["path"]) for _ in range(5)]

    assert sorted(p for p in tmp_path.glob("*.png") if p != unrelated) == saved[-3:]
    assert unrelated.is_file()
