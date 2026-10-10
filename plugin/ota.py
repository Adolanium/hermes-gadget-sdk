"""Over-the-air firmware updates: check an image, then stream it to a device.

The device authorizes each update with its own key. It answers an offer with a
fresh nonce, and the hub proves it holds the key enrolled for that device by
sending back HMAC(key, nonce, the image's SHA-256, its size). The image then
streams over binary channel 3, acknowledged every 16 KB. The device checks the
size and the SHA-256, and its port checks the image format, before it switches
to the new firmware. A new firmware must reach Hermes within five minutes of
its first boot, or the device goes back to the previous one.

``hermes gadget update`` runs in its own process, so it stages images in an
``UpdateQueue`` that the gateway's adapter picks up.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from . import protocol

PROJECT = "hermes_gadget"     # the ESP-IDF project name in every Hermes Gadget image
IMAGE_MAGIC = 0xE9            # esp_image_header_t.magic
APP_DESC_MAGIC = 0xABCD5432   # esp_app_desc_t.magic_word, right after the image and segment headers
APP_DESC_OFFSET = 32
BOARD_TAG = b"HGBOARD="       # the firmware carries "HGBOARD=<board>" (firmware/esp32/main/board.cpp)
CHUNK_BYTES = 4096
WINDOW_BYTES = 64 * 1024      # unacknowledged data in flight
REPLY_TIMEOUT_S = 30.0
DONE_TIMEOUT_S = 60.0         # checking the whole image takes a few seconds on the device
RETRY_CODES = {"disconnected", "timeout"}  # worth another try when the device comes back
MAX_ATTEMPTS = 3

Progress = Callable[[int, int], None]


class UpdateError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass(frozen=True)
class FirmwareImage:
    data: bytes
    version: str
    board: Optional[str]
    sha256: str

    @property
    def size(self) -> int:
        return len(self.data)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def inspect_image(data: bytes) -> FirmwareImage:
    """Check that ``data`` is a Hermes Gadget app image (``firmware.bin``) and read its version and board."""
    if len(data) < APP_DESC_OFFSET + 256 or data[0] != IMAGE_MAGIC:
        raise UpdateError("not_an_image", "not an ESP32 app image; use firmware.bin from the build")
    if struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise UpdateError("not_an_image", "the image has no app description; use firmware.bin from the build")
    project = _cstr(data[80:112])
    if project != PROJECT:
        raise UpdateError("wrong_firmware", f"the image is {project!r}, not Hermes Gadget firmware")
    board = None
    tag = data.find(BOARD_TAG)
    while tag >= 0 and board is None:  # older images also hold a nameless copy of the tag
        board = _cstr(data[tag + len(BOARD_TAG): tag + len(BOARD_TAG) + 64]) or None
        tag = data.find(BOARD_TAG, tag + 1)
    return FirmwareImage(data=data, version=_cstr(data[48:80]), board=board,
                         sha256=hashlib.sha256(data).hexdigest())


def check_for(image: FirmwareImage, session) -> None:
    """Refuse an image the device can't take, before anything is sent."""
    ota = session.caps.get("ota")
    if not isinstance(ota, dict):
        raise UpdateError("unsupported", f"{session.name} runs firmware without over-the-air updates; "
                                         "flash this image over USB once")
    max_size = int(ota.get("max_size") or 0)
    if max_size and image.size > max_size:
        raise UpdateError("too_large", f"the image is {image.size} bytes; {session.name}'s update slot "
                                       f"holds {max_size}")
    if image.board and session.board and image.board != session.board:
        raise UpdateError("wrong_board", f"the image is built for {image.board}, but {session.name} "
                                         f"is {session.board}")


async def send_update(session, image: FirmwareImage, key: bytes, *, progress: Optional[Progress] = None) -> str:
    """Stream ``image`` to ``session``; returns the version the device installed.

    Raises UpdateError with the device's reason when it refuses or the update fails.
    """
    if session._ota_inbox is not None:
        raise UpdateError("busy", f"{session.name} is already installing an update")
    inbox: asyncio.Queue = asyncio.Queue()
    session._ota_inbox = inbox
    stream = session._next_stream()
    acked = 0

    def take(msg: dict, want: str) -> dict:
        nonlocal acked
        kind = msg.get("type")
        if kind == "ota.error":
            raise UpdateError(str(msg.get("code") or "error"), str(msg.get("message") or "the device refused the update"))
        if kind == "ota.ack":
            acked = max(acked, int(msg.get("offset") or 0))
            if progress:
                progress(acked, image.size)
        if want != "ota.ack" and kind == "ota.ack":
            return {}
        if kind != want:
            raise UpdateError("protocol", f"expected {want} from the device, got {kind}")
        return msg

    async def expect(want: str, timeout: float = REPLY_TIMEOUT_S) -> dict:
        while True:
            try:
                msg = await asyncio.wait_for(inbox.get(), timeout)
            except asyncio.TimeoutError:
                raise UpdateError("timeout", f"{session.name} didn't answer within {timeout:.0f} s") from None
            got = take(msg, want)
            if got:
                return got

    try:
        await session.send_json(protocol.message(
            "ota.offer", stream=stream, size=image.size, sha256=image.sha256, version=image.version))
        nonce = str((await expect("ota.ready")).get("nonce") or "")
        mac = protocol.ota_mac(key, session.device_id, nonce, image.sha256, image.size)
        await session.send_json(protocol.message("ota.begin", mac=mac))
        await expect("ota.ack")
        sent = 0
        for seq, off in enumerate(range(0, image.size, CHUNK_BYTES)):
            while sent - acked >= WINDOW_BYTES:
                await expect("ota.ack")
            chunk = image.data[off: off + CHUNK_BYTES]
            await session.send_binary(protocol.binary(protocol.CHANNEL_FIRMWARE, stream, seq, chunk))
            sent += len(chunk)
            while not inbox.empty():  # acknowledgements (or an error) that already arrived
                take(inbox.get_nowait(), "ota.ack")
        while acked < image.size:
            await expect("ota.ack")
        await session.send_json(protocol.message("ota.end"))
        done = await expect("ota.done", DONE_TIMEOUT_S)
        return str(done.get("version") or image.version)
    except UpdateError as exc:
        if exc.code in ("timeout", "protocol"):
            await _abort(session)
        raise
    except asyncio.CancelledError:
        await _abort(session)
        raise
    except Exception as exc:
        from websockets.exceptions import ConnectionClosed

        if isinstance(exc, ConnectionClosed) or session.closed:
            raise UpdateError("disconnected", f"{session.name} disconnected during the update") from exc
        raise
    finally:
        session._ota_inbox = None


async def _abort(session) -> None:
    try:
        await session.send_json(protocol.message("ota.abort"))
    except Exception:
        pass


class UpdateQueue:
    """Firmware staged by ``hermes gadget update`` until the gateway installs it.

    The CLI and the gateway are separate processes and meet in files under the
    plugin's data directory, in ``updates/``: ``<device>.bin`` (the image),
    ``<device>.json`` (what was staged, attempts so far) and ``<device>.status.json``
    (what the gateway reports back).
    """

    def __init__(self, directory: Path | str):
        self.dir = Path(directory) / "updates"

    def _path(self, device_id: str, suffix: str) -> Path:
        if not protocol.DEVICE_ID_RE.match(device_id):
            raise ValueError(f"not a device id: {device_id!r}")
        return self.dir / f"{device_id}{suffix}"

    def _write(self, path: Path, data: bytes) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, path)

    def stage(self, device_id: str, image: FirmwareImage) -> None:
        self._write(self._path(device_id, ".bin"), image.data)
        meta = {"version": image.version, "sha256": image.sha256, "size": image.size, "board": image.board,
                "staged_at": time.time(), "attempts": 0}
        self._write(self._path(device_id, ".json"), json.dumps(meta).encode())
        self._path(device_id, ".status.json").unlink(missing_ok=True)

    def pending(self) -> list[str]:
        if not self.dir.is_dir():
            return []
        return sorted(p.stem for p in self.dir.glob("*.bin") if protocol.DEVICE_ID_RE.match(p.stem))

    def load(self, device_id: str) -> Optional[tuple[bytes, dict]]:
        try:
            data = self._path(device_id, ".bin").read_bytes()
            meta = json.loads(self._path(device_id, ".json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data, meta if isinstance(meta, dict) else {}

    def count_attempt(self, device_id: str) -> int:
        staged = self.load(device_id)
        if staged is None:
            return 0
        meta = staged[1]
        meta["attempts"] = int(meta.get("attempts") or 0) + 1
        self._write(self._path(device_id, ".json"), json.dumps(meta).encode())
        return meta["attempts"]

    def drop(self, device_id: str) -> None:
        for suffix in (".bin", ".json"):
            self._path(device_id, suffix).unlink(missing_ok=True)

    def report(self, device_id: str, **status) -> None:
        self._write(self._path(device_id, ".status.json"), json.dumps({**status, "at": time.time()}).encode())

    def status(self, device_id: str) -> Optional[dict]:
        try:
            status = json.loads(self._path(device_id, ".status.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return status if isinstance(status, dict) else None
