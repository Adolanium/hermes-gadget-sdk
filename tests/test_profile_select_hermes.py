"""Profile switching through the native simulator."""
from __future__ import annotations

import pytest
from conftest import requires_hermes, requires_sim
from hermes_fixtures import _paired_sim

pytestmark = [requires_hermes, requires_sim, pytest.mark.usefixtures("registered_platform")]


def _select_next_profile(sim):
    sim.console("settings")
    for _ in range(3):
        sim.console("cancel")
    sim.console("talk")
    sim.console("release")
    sim.console("settings close")


def _profile_msgs(sim):
    return [m for m in sim.received if m["type"] == "profile"]


def test_a_paired_device_switches_profile_and_its_turns_run_there(hermes_home, gadget, make_sim):
    from gateway.pairing import PairingStore

    sim = _paired_sim(gadget, make_sim)
    device = sim.status()["device_id"]
    assert [p["id"] for p in sim.last_received("welcome")["profiles"]] == ["default", "ops"]
    _select_next_profile(sim)
    assert sim.wait_for(lambda: {"type": "profile", "profile": "ops"} in _profile_msgs(sim), timeout=5)
    assert not PairingStore(profile="ops").is_approved("gadget", device)  # nothing written into ops
    gadget.events.clear()
    sim.type_text("hi")
    assert sim.wait_for(lambda: gadget.events, timeout=5)
    assert gadget.events[-1].source.profile == "ops"


def test_a_capable_device_on_default_says_so_explicitly(hermes_home, gadget, make_sim):
    # An explicit "default" stops a static gateway.profile_routes entry from overriding the device.
    sim = _paired_sim(gadget, make_sim)
    gadget.events.clear()
    sim.type_text("hi")
    assert sim.wait_for(lambda: gadget.events, timeout=5)
    assert gadget.events[-1].source.profile == "default"


def test_switching_is_refused_when_unknown_unpaired_or_busy(hermes_home, gadget, make_sim):
    import time as _time

    from hermes_gadget_plugin.adapter import _Prompt

    run, adapter = gadget.run, gadget.adapter
    sim = make_sim(gadget.url)
    assert sim.wait_screen("pairing", timeout=10)
    session = adapter.hub.get(sim.status()["device_id"])
    assert run(adapter.select_profile(session, "ops")) == "unpaired"
    gadget.authorized.add(session.device_id)
    assert sim.wait_screen("ready", timeout=10)
    assert run(adapter.select_profile(session, "nosuch")) == "unknown"
    # A device that never said it can switch (no caps.profiles) is refused, whatever it asks for.
    caps = dict(session.caps)
    session.caps.pop("profiles", None)
    assert run(adapter.select_profile(session, "ops")) == "unknown"
    assert session.profile != "ops"
    session.caps.update(caps)
    assert run(adapter.select_profile(session, "old")) == "unknown"  # parked
    adapter._turns[session.device_id] = "t1"
    assert run(adapter.select_profile(session, "ops")) == "busy"
    adapter._turns.pop(session.device_id)
    # A question pending -> busy; once it has expired, switching works again.
    stale = _Prompt(id="q1", kind="slash", session_key="k", title="t", text="x", ttl_s=300)
    adapter._prompts[session.device_id] = [stale]
    assert run(adapter.select_profile(session, "ops")) == "busy"
    stale.created = _time.monotonic() - 10_000
    assert run(adapter.select_profile(session, "ops")) is None
    assert session.profile == "ops"


def test_a_stored_profile_that_is_gone_falls_back_to_default(hermes_home, gadget, make_sim):
    sim = _paired_sim(gadget, make_sim)
    _select_next_profile(sim)
    assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
    first = sim.last_received("welcome")["session"]
    (hermes_home / "profiles" / "ops" / "gateway.parked").touch()  # parked while connected
    assert sim.wait_for(lambda: _profile_msgs(sim)[-1] == {"type": "profile", "profile": "default"}, timeout=10)
    sim.console("reconnect")
    assert sim.wait_for(lambda: sim.last_received("welcome")["session"] != first, timeout=10)
    assert sim.last_received("welcome")["profile"] == "default"
    assert sim.status().get("profile") == "default"


def test_revocation_resets_profile_and_questions_even_mid_switch(hermes_home, gadget, make_sim, monkeypatch):
    from hermes_gadget_plugin.adapter import _Prompt

    run, adapter = gadget.run, gadget.adapter
    sim = _paired_sim(gadget, make_sim)
    session = adapter.hub.get(sim.status()["device_id"])

    bind = adapter._store.bind_to_owner

    def revoke_during_bind(device_id):
        gadget.authorized.discard(device_id)  # revoked on the default profile while the switch runs
        return bind(device_id)

    with monkeypatch.context() as bind_patch:  # never monkeypatch.undo(): it would drop the fixtures' patches too
        bind_patch.setattr(adapter._store, "bind_to_owner", revoke_during_bind)
        assert run(adapter.select_profile(session, "ops")) == "unpaired"
    assert session.profile == "default"
    gadget.authorized.add(session.device_id)
    assert sim.wait_screen("ready", timeout=10)
    _select_next_profile(sim)
    assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
    adapter._prompts[session.device_id] = [_Prompt(id="q1", kind="approval", session_key="k", title="t", text="x")]
    gadget.authorized.discard(session.device_id)
    assert sim.wait_for(lambda: sim.status().get("profile") == "default", timeout=10)
    assert not adapter._prompts.get(session.device_id)
    resolved = []
    monkeypatch.setattr("tools.approval.resolve_gateway_approval", lambda *a, **kw: resolved.append(a))
    run(adapter.on_prompt_reply(session, "q1", True))  # ignored: the device is not approved
    assert resolved == []


def test_switching_trusts_only_a_definite_yes_from_the_default_profile(hermes_home, gadget, make_sim):
    from gateway.pairing import PairingStore

    run, adapter = gadget.run, gadget.adapter
    sim = _paired_sim(gadget, make_sim)
    session = adapter.hub.get(sim.status()["device_id"])

    def boom(*a, **kw):
        raise RuntimeError("auth backend down")

    for verdict in (lambda *a, **kw: None, boom, lambda *a, **kw: "yes"):  # unknown, raising, non-boolean
        adapter._owner_check = verdict
        assert run(adapter.select_profile(session, "ops")) == "unpaired"
    assert not PairingStore(profile="ops").is_approved("gadget", session.device_id)  # nothing minted
    # Approved only in ops (for example through an old static route) does not count as trusted.
    ops = PairingStore(profile="ops")
    ops.approve_code("gadget", ops.generate_code("gadget", session.device_id, "Kitchen"))
    adapter._owner_check = lambda user_id, *a, **kw: PairingStore(profile="default").is_approved("gadget", user_id)
    assert run(adapter.select_profile(session, "ops")) == "unpaired"


def test_a_turn_marker_does_not_survive_revocation(hermes_home, gadget, make_sim):
    run, adapter = gadget.run, gadget.adapter
    sim = _paired_sim(gadget, make_sim)
    session = adapter.hub.get(sim.status()["device_id"])
    _select_next_profile(sim)
    assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
    adapter._turns[session.device_id] = "t1"
    gadget.authorized.discard(session.device_id)
    assert sim.wait_for(lambda: sim.status().get("profile") == "default", timeout=10)
    gadget.authorized.add(session.device_id)
    assert sim.wait_screen("ready", timeout=10)
    assert run(adapter.select_profile(session, "ops")) is None  # not stuck on "busy"


def test_one_closed_socket_does_not_stop_the_resets(hermes_home, gadget, make_sim, monkeypatch):
    from websockets.exceptions import ConnectionClosedOK

    adapter = gadget.adapter
    first = _paired_sim(gadget, make_sim, state="first")
    second = _paired_sim(gadget, make_sim, name="Second", state="second")
    for sim in (first, second):
        _select_next_profile(sim)
        assert sim.wait_for(lambda: sim.status().get("profile") == "ops", timeout=5)
    one, two = (adapter.hub.get(sim.status()["device_id"]) for sim in (first, second))
    if sorted(adapter.hub.sessions) != [one.device_id, two.device_id]:  # make the broken one come first
        one, two, first, second = two, one, second, first

    async def closed(_obj):
        raise ConnectionClosedOK(None, None)

    monkeypatch.setattr(one, "send_json", closed)
    gadget.authorized.discard(one.device_id)
    gadget.authorized.discard(two.device_id)
    assert second.wait_for(lambda: second.status().get("profile") == "default", timeout=10)
    gadget.authorized.add(two.device_id)
    assert second.wait_screen("ready", timeout=10)  # the watcher still notices approvals


def test_a_prompt_reply_needs_live_trust_and_the_current_connection(hermes_home, gadget, make_sim, monkeypatch):
    from hermes_gadget_plugin.adapter import _Prompt

    run, adapter = gadget.run, gadget.adapter
    sim = _paired_sim(gadget, make_sim)
    adapter._watch_task.cancel()  # no poll from here on: the cached paired flag stays True
    session = adapter.hub.get(sim.status()["device_id"])
    resolved = []
    monkeypatch.setattr("tools.approval.resolve_gateway_approval", lambda *a, **kw: resolved.append(a) or True)

    def ask(prompt_id):
        adapter._prompts[session.device_id] = [
            _Prompt(id=prompt_id, kind="approval", session_key="k", title="t", text="x")]

    ask("q1")
    adapter._owner_check = lambda *a, **kw: False  # revoked on the default profile, not yet polled
    assert session.paired
    run(adapter.on_prompt_reply(session, "q1", True))
    assert resolved == []
    adapter._owner_check = lambda *a, **kw: True
    sim.console("reconnect")  # the same device on a new connection
    assert sim.wait_for(lambda: adapter.hub.get(session.device_id) not in (None, session), timeout=10)
    ask("q2")
    run(adapter.on_prompt_reply(session, "q2", True))  # the old connection answers
    assert resolved == []
    current = adapter.hub.get(session.device_id)
    ask("q3")
    run(adapter.on_prompt_reply(current, "q3", True))
    assert len(resolved) == 1


def test_a_refused_message_does_not_leave_the_device_thinking(hermes_home, gadget, make_sim):
    run, adapter = gadget.run, gadget.adapter
    sim = _paired_sim(gadget, make_sim)
    session = adapter.hub.get(sim.status()["device_id"])
    assert run(adapter.select_profile(session, "ops")) is None
    adapter._watch_task.cancel()  # no poll: only the refusal itself can end the wait
    adapter._owner_check = lambda *a, **kw: False
    gadget.events.clear()
    sim.type_text("hi")
    assert sim.wait_for(lambda: sim.status()["screen"] == "ready", timeout=3)
    assert gadget.events == []
    assert "not approved" in (sim.last_received("notice") or {}).get("text", "")


def test_each_profile_shows_its_own_face_file_and_profile_faces_false_turns_them_off(hermes_home, gadget, make_sim):
    import struct

    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter
    from hermes_gadget_plugin.face import FACE_BYTES, FACE_FILE, ProfileFace

    anchors = struct.pack("<10h", 607, 293, 262, 620, 801, 107, 396, 508, 586, 649)
    face = {"default": bytes([0xAA]) * (FACE_BYTES - 20) + anchors, "ops": bytes([0x55]) * (FACE_BYTES - 20) + anchors}
    (hermes_home / FACE_FILE).write_bytes(face["default"])
    (hermes_home / "profiles" / "ops" / FACE_FILE).write_bytes(face["ops"])
    digest = {p: ProfileFace.of(data).hash for p, data in face.items()}
    faces = lambda: [m for m in sim.received if m["type"] == "face"]  # noqa: E731

    def held():
        """The face the device holds, as the hello of a reconnect names it."""
        sim.console("reconnect")
        assert sim.wait_screen("connecting", timeout=5)
        assert sim.wait_screen("ready", timeout=10)
        return [m for m in sim.sent if m["type"] == "hello"][-1].get("face")

    sim = _paired_sim(gadget, make_sim)
    assert sim.wait_for(lambda: faces() and faces()[-1]["hash"] == digest["default"], timeout=5)
    assert held() == digest["default"]
    _select_next_profile(sim)
    assert sim.wait_for(lambda: faces()[-1]["hash"] == digest["ops"], timeout=5)
    assert faces()[-1]["profile"] == "ops"
    assert held() == digest["ops"]

    gadget.adapter._profile_faces = False  # as profile_faces: false sets it
    assert held() == digest["ops"]
    assert "face" not in sim.last_received("welcome")  # this reconnect's welcome drops it
    assert held() is None
    assert GadgetAdapter(PlatformConfig(enabled=True, extra={"profile_faces": False}))._profile_faces is False
