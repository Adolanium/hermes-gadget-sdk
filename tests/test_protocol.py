"""Server-side protocol helpers agree with the firmware (same vectors as
firmware/tests/test_basics.cpp)."""

from hermes_gadget_plugin import protocol

KEY = bytes(range(32))
NONCE = "bm9uY2Utbm9uY2Utbm9uY2U="


def test_device_id_and_mac_match_firmware_vectors():
    device_id = protocol.device_id_for_key(KEY)
    assert device_id == "hg-630dcd2966c43366"
    assert protocol.auth_mac(KEY, device_id, NONCE) == "AMUEF53Phk8+1vHw7R8PgiDwlPd8rXUx+cgyYv1YJX0="
    empty_sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    assert (protocol.ota_mac(KEY, device_id, "AAECAwQFBgcICQoLDA0ODw==", empty_sha, 1234)
            == "ieymn+y1CMEJ5uX8yGz8VrXxT2KKjSOnzJC5qagTTwk=")


def test_mac_verification_rejects_wrong_key_nonce_or_device():
    device_id = protocol.device_id_for_key(KEY)
    mac = protocol.auth_mac(KEY, device_id, NONCE)
    assert protocol.verify_mac(KEY, device_id, NONCE, mac)
    assert not protocol.verify_mac(bytes(32), device_id, NONCE, mac)
    assert not protocol.verify_mac(KEY, device_id, "other", mac)
    assert not protocol.verify_mac(KEY, "hg-0000000000000000", NONCE, mac)
    assert not protocol.verify_mac(KEY, device_id, NONCE, "")


def test_binary_frames_round_trip_and_reject_unknown_channels():
    frame = protocol.binary(protocol.CHANNEL_AUDIO, 7, 0x1234, b"\x01\x02")
    parsed = protocol.parse_binary(frame)
    assert (parsed.channel, parsed.stream, parsed.seq, parsed.payload) == (protocol.CHANNEL_AUDIO, 7, 0x1234, b"\x01\x02")
    assert protocol.parse_binary(b"\x09\x00\x00\x00") is None
    assert protocol.parse_binary(b"\x01") is None


def test_key_decoding_requires_exactly_32_bytes():
    import base64

    assert protocol.decode_key(base64.b64encode(KEY).decode()) == KEY
    assert protocol.decode_key(base64.b64encode(KEY[:16]).decode()) is None
    assert protocol.decode_key("not base64!") is None
