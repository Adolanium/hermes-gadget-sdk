"""Hermes-independent plugin modules: text shaping, audio, device store."""

import asyncio
import io
import json
import math
import struct
import wave

import pytest

from hermes_gadget_plugin import audio, textfmt
from hermes_gadget_plugin.store import DeviceStore


# -- text --------------------------------------------------------------------------------

def test_markdown_is_flattened_for_small_screens():
    md = "# Title\n\n**Bold** and *italic* and `code`.\n\n- one\n- two\n\n[link text](https://x.y)\n\n---"
    out = textfmt.for_device(md)
    assert "#" not in out and "**" not in out and "`" not in out and "](" not in out
    assert "Bold and italic and code." in out
    assert "* one" in out and "link text" in out


def test_typography_and_accents_fold_to_ascii_and_emoji_drop():
    out = textfmt.for_device("Café “quoted” — it’s 20°C \U0001F600")
    assert out == "Cafe \"quoted\" - it's 20 degC"


def test_a_latin1_device_keeps_its_accents_and_folds_the_rest():
    out = textfmt.for_device("¿Café “quoted” — it’s 20°C, Ñandú? Łódź 日本 \U0001F600", "latin1")
    assert out == "¿Café \"quoted\" - it's 20°C, Ñandú? Lódz"


def test_a_latin1_device_gets_plain_spaces_and_no_soft_hyphens():
    assert textfmt.for_device("a\xa0b co\xadop\tx", "latin1") == "a b coop x"


def test_an_unknown_charset_is_left_alone():
    # A device announcing something the plugin doesn't know draws what it can itself.
    assert textfmt.for_device("Café \U0001F600", "utf-8") == "Café \U0001F600"


def test_think_blocks_never_reach_the_screen():
    assert textfmt.for_device("<think>secret plan</think>Answer") == "Answer"


def test_code_fences_keep_their_content():
    assert textfmt.for_device("```python\nprint(1)\n```") == "print(1)"


# -- audio -------------------------------------------------------------------------------

def _sine(rate: int, seconds: float, freq: float = 440.0) -> bytes:
    return b"".join(struct.pack("<h", int(10000 * math.sin(2 * math.pi * freq * i / rate)))
                    for i in range(int(rate * seconds)))


@pytest.mark.parametrize("src,dst", [(24000, 16000), (16000, 24000), (22050, 16000), (16000, 16000)])
def test_resampler_output_length_tracks_the_rate_ratio(src, dst):
    pcm = _sine(src, 1.0)
    out = audio.resample(pcm, src, dst)
    assert abs(len(out) // 2 - dst) <= 2


def test_chunked_resampling_equals_one_shot():
    pcm = _sine(24000, 0.5)
    whole = audio.resample(pcm, 24000, 16000)
    r = audio.Resampler(24000, 16000)
    pieces = b"".join(r.process(pcm[i:i + 777]) for i in range(0, len(pcm), 777))  # odd sizes split samples
    assert len(pieces) == len(whole)
    a = struct.unpack(f"<{len(whole) // 2}h", whole)
    b = struct.unpack(f"<{len(pieces) // 2}h", pieces)
    assert max(abs(x - y) for x, y in zip(a, b)) <= 1


def test_stereo_input_is_downmixed():
    mono = _sine(16000, 0.1)
    stereo = b"".join(mono[i:i + 2] * 2 for i in range(0, len(mono), 2))
    assert audio.Resampler(16000, 16000, channels=2).process(stereo) == mono


def test_wav_round_trip_through_decode(tmp_path):
    pcm = _sine(24000, 0.25)
    path = tmp_path / "a.wav"
    path.write_bytes(audio.wav_bytes(pcm, 24000))
    decoded = audio.decode_file(str(path), 16000)
    assert abs(len(decoded) // 2 - 4000) <= 2
    with wave.open(io.BytesIO(audio.wav_bytes(pcm, 24000))) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (24000, 1, 2)


# -- store -------------------------------------------------------------------------------

def test_store_persists_enrollment_and_hides_keys(tmp_path):
    store = DeviceStore(tmp_path)
    store.enroll("hg-0123456789abcdef", b"k" * 32, name="Kitchen", board="b")
    again = DeviceStore(tmp_path)
    assert again.key_for("hg-0123456789abcdef") == b"k" * 32
    listed = again.devices()["hg-0123456789abcdef"]
    assert listed["name"] == "Kitchen" and "key" not in listed
    assert again.forget("hg-0123456789abcdef")
    assert DeviceStore(tmp_path).key_for("hg-0123456789abcdef") is None


def test_pairing_codes_expire(tmp_path):
    store = DeviceStore(tmp_path)
    store.remember_pairing("hg-0123456789abcdef", "ABCD2345", "hermes pairing approve gadget ABCD2345", ttl_s=60)
    assert store.pairing_for("hg-0123456789abcdef")[0] == "ABCD2345"
    store.remember_pairing("hg-0123456789abcdef", "ABCD2345", "cmd", ttl_s=-1)
    assert store.pairing_for("hg-0123456789abcdef") is None


def test_store_changes_from_another_process_are_seen(tmp_path):
    """The gateway keeps its DeviceStore open; `hermes gadget forget` edits the same file from another process."""
    gateway = DeviceStore(tmp_path)
    gateway.enroll("hg-0123456789abcdef", b"k" * 32, name="Kitchen", board="b")

    cli = DeviceStore(tmp_path)  # a second process: fresh instance on the same file
    assert cli.forget("hg-0123456789abcdef")

    gateway.touch("hg-0123456789abcdef", firmware="0.2.0")  # the device reconnects
    assert gateway.key_for("hg-0123456789abcdef") is None, "a forgotten key must not be resurrected by the gateway"
    assert DeviceStore(tmp_path).key_for("hg-0123456789abcdef") is None

    cli.enroll("hg-fedcba9876543210", b"j" * 32)  # and the other way round
    assert gateway.key_for("hg-fedcba9876543210") == b"j" * 32


def test_an_unapproved_device_expires_and_an_approved_one_stays(tmp_path, monkeypatch):
    from hermes_gadget_plugin import store as storemod

    now = {"t": 1_000_000.0}
    monkeypatch.setattr(storemod.time, "time", lambda: now["t"])
    store = DeviceStore(tmp_path)
    store.enroll("hg-0123456789abcdef", b"k" * 32, name="Stranger", address="10.0.0.9")
    store.enroll("hg-fedcba9876543210", b"j" * 32, name="Mine")
    store.confirm("hg-fedcba9876543210")
    assert set(store.pending()) == {"hg-0123456789abcdef"}
    assert store.devices()["hg-0123456789abcdef"]["pending"] is True
    assert store.devices()["hg-fedcba9876543210"]["pending"] is False

    now["t"] += storemod.PENDING_TTL_S - 1
    store.touch("hg-0123456789abcdef")  # still in contact: the clock restarts
    now["t"] += storemod.PENDING_TTL_S - 1
    assert store.key_for("hg-0123456789abcdef") == b"k" * 32

    now["t"] += 2  # no contact for a whole TTL
    assert store.key_for("hg-0123456789abcdef") is None, "an unapproved record is gone after its TTL"
    assert "hg-0123456789abcdef" not in store.devices()
    assert store.key_for("hg-fedcba9876543210") == b"j" * 32, "an approved record stays"


def test_battery_log_writes_a_row_per_interval_and_at_once_on_a_power_change(tmp_path):
    import csv

    from hermes_gadget_plugin.battery_log import BatteryLog

    now = [1_700_000_000.0]
    log = BatteryLog(tmp_path, clock=lambda: now[0], interval_s=30)
    reading = {"battery_mv": 3950, "battery_percent": 72, "charging": 0, "external_power": 0, "battery_present": 1}
    assert log.record("hg-1/../x", reading) == tmp_path / "hg-1_.._x.csv"  # the id cannot leave the directory
    now[0] += 10
    assert log.record("hg-1/../x", {**reading, "battery_mv": 3949}) is None  # not due yet
    now[0] += 5
    assert log.record("hg-1/../x", {**reading, "charging": 1, "external_power": 1}) is not None  # USB plugged in
    now[0] += 30
    assert log.record("hg-1/../x", {**reading, "battery_mv": 3990, "charging": 1, "external_power": 1}) is not None
    assert log.record("hg-1/../x", {"temperature": 21}) is None  # no battery reading: nothing to log
    rows = list(csv.DictReader((tmp_path / "hg-1_.._x.csv").open()))
    assert [r["battery_mv"] for r in rows] == ["3950", "3950", "3990"]
    assert [r["charging"] for r in rows] == ["0", "1", "1"]
    assert rows[0]["unix_s"] == "1700000000" and rows[0]["battery_percent"] == "72"


def test_battery_log_rolls_over_a_full_file(tmp_path):
    from hermes_gadget_plugin.battery_log import BatteryLog

    now = [0.0]
    log = BatteryLog(tmp_path, clock=lambda: now[0], interval_s=1, max_bytes=200)
    for i in range(20):
        now[0] += 1
        log.record("hg-1", {"battery_mv": 4000 - i})
    assert (tmp_path / "hg-1.1.csv").exists()
    assert (tmp_path / "hg-1.csv").stat().st_size < 400
    assert (tmp_path / "hg-1.csv").read_text().startswith("time,unix_s,battery_mv")


def test_owner_binding_survives_forget_from_a_stale_store(tmp_path):
    device = "hg-0123456789abcdef"
    gateway = DeviceStore(tmp_path)
    gateway.enroll(device, b"k" * 32)
    cli = DeviceStore(tmp_path)  # another process
    assert not gateway.owner_bound(device)
    gateway.bind_to_owner(device)
    gateway.bind_to_owner(device)  # idempotent
    assert cli.forget(device)  # deletes the device's record; it is not a revocation
    assert DeviceStore(tmp_path).owner_bound(device)


# -- hub: utterances that end without audio ---------------------------------------------


class _FakeDevice:
    """The slice of DeviceSession the hub's audio path touches; records what it would send."""

    name = "Desk"
    device_id = "hg-0000000000000000"

    def __init__(self, caps=None):
        self.caps = caps if caps is not None else {}
        self.sent = []
        self._upload = None

    async def send_notice(self, text, ttl_s=8):
        self.sent.append(("notice", text))

    async def turn_end(self, turn, outcome="success"):
        self.sent.append(("turn.end", turn, outcome))

    def spawn(self, coro):
        return asyncio.get_running_loop().create_task(coro)


def test_a_too_short_utterance_ends_the_turn_so_the_device_settles():
    from hermes_gadget_plugin import hub

    async def run():
        device = _FakeDevice(caps={"inputs": ["talk"], "talk_mode": "hold"})
        device._upload = hub._Upload(id="a1", stream=4, rate=16000, started=0.0, pcm=bytearray(3200))
        await hub.DeviceHub(None, hub.HubDelegate())._h_audio_end(device, {"stream": 4})
        assert device.sent == [("notice", "Didn't catch that - hold the button while speaking"),
                               ("turn.end", "", "cancelled")]

    asyncio.run(run())


def test_an_end_with_no_audio_also_ends_the_turn():
    from hermes_gadget_plugin import hub

    async def run():
        device = _FakeDevice()
        await hub.DeviceHub(None, hub.HubDelegate())._h_audio_end(device, {"stream": 9})
        assert ("turn.end", "", "cancelled") in device.sent

    asyncio.run(run())


@pytest.mark.parametrize("caps, notice", [
    ({"inputs": ["talk", "touch"], "talk_mode": "tap"}, "Didn't catch that - tap and speak"),
    ({"inputs": ["talk", "touch"], "talk_mode": "hold"}, "Didn't catch that - hold the screen while speaking"),
    ({"inputs": ["talk", "cancel"], "talk_mode": "hold"}, "Didn't catch that - hold the button while speaking"),
])
def test_the_retry_hint_names_the_control_the_device_uses(caps, notice):
    from hermes_gadget_plugin import hub

    async def run():
        device = _FakeDevice(caps=caps)
        device._upload = hub._Upload(id="a3", stream=4, rate=16000, started=0.0, pcm=bytearray(800))
        await hub.DeviceHub(None, hub.HubDelegate())._h_audio_end(device, {"stream": 4})
        assert ("notice", notice) in device.sent

    asyncio.run(run())


def test_long_enough_utterance_still_reaches_the_delegate():
    from hermes_gadget_plugin import hub

    heard = []

    class Delegate(hub.HubDelegate):
        async def on_utterance(self, session, msg_id, wav, seconds):
            heard.append((msg_id, seconds))

    async def run():
        device = _FakeDevice()
        device._upload = hub._Upload(id="a2", stream=4, rate=16000, started=0.0, pcm=bytearray(16000))
        await hub.DeviceHub(None, Delegate())._h_audio_end(device, {"stream": 4})
        await asyncio.sleep(0)
        assert heard == [("a2", 0.5)]
        assert device.sent == []

    asyncio.run(run())


# -- faces -------------------------------------------------------------------------------

class _Wire:
    """A device's socket: records what the hub sends."""

    remote_address = ("127.0.0.1", 1)

    def __init__(self):
        self.sent = []

    async def send(self, raw):
        self.sent.append(json.loads(raw))


def _slow_face_session(tmp_path, hub=None):
    """A paired, current session on default whose face is made only once ``delegate.release`` is set."""
    from hermes_gadget_plugin.face import ProfileFace
    from hermes_gadget_plugin.hub import DeviceHub, DeviceSession, HubDelegate

    class SlowDefault(HubDelegate):
        def __init__(self):
            self.release = asyncio.Event()

        async def face(self, session):
            if session.profile != "default":
                return None
            await self.release.wait()
            return ProfileFace.of(b"the default profile's face")

    delegate, wire = SlowDefault(), _Wire()
    hub = hub or DeviceHub(DeviceStore(tmp_path), delegate)
    session = DeviceSession(hub, wire, {"device_id": "hg-1", "caps": {"faces": 1}})
    session.paired = True
    hub.sessions[session.device_id] = session
    return session, hub.delegate, wire


def test_a_face_that_arrives_after_a_profile_switch_is_not_sent(tmp_path):
    """The switch sends the new profile's face; a slow face for the old one must never follow it."""

    async def main():
        session, delegate, wire = _slow_face_session(tmp_path)
        first = session.update_face()  # as after welcome, on default
        await asyncio.sleep(0)
        session.profile = "ops"
        await session.send_profile()
        delegate.release.set()
        await asyncio.gather(first, session._face_task, return_exceptions=True)
        return wire.sent

    assert asyncio.run(main()) == [{"type": "profile", "profile": "ops"}]


def test_a_face_that_arrives_after_the_device_is_unpaired_is_not_sent(tmp_path):
    async def main():
        session, delegate, wire = _slow_face_session(tmp_path)
        first = session.update_face()
        await asyncio.sleep(0)
        await session.set_paired(False)  # revoked while the face was being made
        delegate.release.set()
        await asyncio.gather(first, session._face_task, return_exceptions=True)
        return wire.sent, session.face_hash

    assert asyncio.run(main()) == ([{"type": "unpaired"}], None)


def test_a_face_made_for_a_replaced_connection_is_dropped(tmp_path):
    """A reconnect replaces the connection while its face is still being made: only the new one answers."""

    async def main():
        old, delegate, old_wire = _slow_face_session(tmp_path)
        first = old.update_face()
        await asyncio.sleep(0)
        new, _, new_wire = _slow_face_session(tmp_path, old.hub)  # the hub's sessions now name the new one
        await old.close("replaced by a newer connection")
        delegate.release.set()
        await asyncio.gather(first, new.update_face(), return_exceptions=True)
        return [m["type"] for m in old_wire.sent], [m["type"] for m in new_wire.sent], new.face_hash

    old_sent, new_sent, held = asyncio.run(main())
    assert "face" not in old_sent and new_sent == ["face"] and held is not None


def test_a_face_found_before_an_unpair_is_not_sent_after_it(tmp_path):
    """The welcome's face was found while paired; an unpair during the welcome's send must not let it go out."""
    from hermes_gadget_plugin.face import ProfileFace

    async def main():
        session, delegate, wire = _slow_face_session(tmp_path)
        found_for = session.face_state()
        found = ProfileFace.of(b"the default profile's face")
        session.paired = False  # revoked while the welcome was being sent
        delegate.release.set()
        await session.update_face(found, found_for)
        return wire.sent

    assert asyncio.run(main()) == []
