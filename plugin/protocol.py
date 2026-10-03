"""Hermes Gadget Protocol v1 — server-side constants and helpers.

The normative description lives in ``docs/protocol.md``; the device side is
``firmware/core/include/hg/protocol.hpp``. Both sides are pinned to the same
test vectors so identity and authentication cannot drift apart.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import re
import struct
from dataclasses import dataclass

VERSION = 1
SUBPROTOCOL = "hermes-gadget.v1"
AUTH_CONTEXT = b"hermes-gadget/v1|"
DEVICE_ID_PREFIX = "hg-"
DEVICE_ID_RE = re.compile(r"^hg-[0-9a-f]{16}$")
KEY_BYTES = 32

BINARY_HEADER = 4
CHANNEL_AUDIO = 0x01
CHANNEL_IMAGE = 0x02


@dataclass(frozen=True)
class BinaryFrame:
    channel: int
    stream: int
    seq: int
    payload: bytes


def parse_binary(data: bytes) -> BinaryFrame | None:
    if len(data) < BINARY_HEADER or data[0] not in (CHANNEL_AUDIO, CHANNEL_IMAGE):
        return None
    channel, stream, seq = struct.unpack_from("<BBH", data)
    return BinaryFrame(channel, stream, seq, bytes(data[BINARY_HEADER:]))


def binary(channel: int, stream: int, seq: int, payload: bytes) -> bytes:
    return struct.pack("<BBH", channel, stream & 0xFF, seq & 0xFFFF) + payload


def device_id_for_key(key: bytes) -> str:
    return DEVICE_ID_PREFIX + hashlib.sha256(key).hexdigest()[:16]


def auth_mac(key: bytes, device_id: str, nonce: str) -> str:
    msg = AUTH_CONTEXT + device_id.encode() + b"|" + nonce.encode()
    return base64.b64encode(hmac.new(key, msg, hashlib.sha256).digest()).decode()


def verify_mac(key: bytes, device_id: str, nonce: str, mac: str) -> bool:
    return hmac.compare_digest(auth_mac(key, device_id, nonce), mac or "")


def decode_key(b64: str) -> bytes | None:
    try:
        key = base64.b64decode(b64 or "", validate=True)
    except (binascii.Error, ValueError):
        return None
    return key if len(key) == KEY_BYTES else None


def message(type_: str, **fields) -> dict:
    return {"type": type_, **fields}
