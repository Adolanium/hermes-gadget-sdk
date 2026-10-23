"""Profile rosters, routing and binding to the default profile, on real Hermes classes."""
from __future__ import annotations

import json
import types
import pytest
from conftest import requires_hermes

pytestmark = [requires_hermes, pytest.mark.usefixtures("registered_platform")]


def test_the_roster_lists_the_profiles_this_gateway_serves(hermes_home, gadget):
    ids = [p["id"] for p in gadget.adapter.profiles()]
    assert ids == ["default", "ops"]  # parked 'old' is not served


@pytest.mark.parametrize("offered, ids", [
    (None, []),  # no key: switching is off
    ("all", ["default", "ops", "lab"]),
    (" ALL ", ["default", "ops", "lab"]),
    (["lab"], ["default", "lab"]),  # the default profile is always offered
    ("lab, ops", ["default"]),  # a list, or "all"; any other string offers no other profile
    (["old", "nosuch"], ["default"]),  # a listed profile still has to be served
    ([], ["default"]),
    ({"lab": True}, ["default"]),  # a malformed list offers no other profile
])
def test_extra_profiles_limits_the_roster(hermes_home, offered, ids):
    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter

    (hermes_home / "profiles" / "lab").mkdir()
    (hermes_home / "profiles" / "lab" / "config.yaml").write_text("{}\n")
    extra = {} if offered is None else {"profiles": offered}
    roster = GadgetAdapter(PlatformConfig(enabled=True, extra=extra)).profiles()
    assert sorted(p["id"] for p in roster) == sorted(ids) and roster[:1] == [r for r in roster if r["id"] == "default"]


def test_without_the_key_a_device_behaves_as_before(hermes_home, tmp_path):
    import asyncio

    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter
    from hermes_gadget_plugin.store import DeviceStore

    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra={}))
    adapter._store = DeviceStore(tmp_path / "plugin-data")
    adapter.set_authorization_check(lambda *a, **kw: True)
    adapter._owner_check = lambda *a, **kw: False  # never asked while switching is off
    session = _session("default")
    assert adapter.profiles(session) == []
    assert adapter._source(session).profile is None  # unstamped, as on main
    assert adapter._verdict(session) is True  # the usual pairing check
    assert asyncio.run(adapter.select_profile(session, "ops")) == "unknown"


def test_a_profile_cannot_add_itself(hermes_home):
    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter

    # Its own config lists it, but only the default profile's adapter config counts.
    (hermes_home / "profiles" / "ops" / "config.yaml").write_text(
        "platforms:\n  gadget:\n    extra:\n      profiles: [ops]\n")
    assert GadgetAdapter(PlatformConfig(enabled=True, extra={})).profiles() == []
    ops_listener = GadgetAdapter(PlatformConfig(enabled=True, extra={"profiles": ["ops"]}))
    ops_listener._owner_profile = "ops"  # Hermes's mark on a profile's own listener
    assert ops_listener.profiles() == []


def test_a_standalone_gateway_offers_no_profiles(hermes_home, gadget, monkeypatch):
    import agent.secret_scope

    monkeypatch.setattr(agent.secret_scope, "is_multiplex_active", lambda: False)
    assert gadget.adapter.profiles() == []


def test_old_firmware_gets_no_roster_and_an_untouched_source(hermes_home, gadget):
    import os
    from hermes_gadget_plugin import protocol

    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    with _RawDevice(gadget.url, key, {}) as device:
        welcome = device.welcome
        device.ws.send(json.dumps({"type": "text", "id": "m1", "text": "hi"}))
        while True:
            msg = json.loads(device.ws.recv(timeout=10))
            if msg.get("type") == "reply" and msg.get("text") == "echo: hi":
                break
    assert "profile" not in welcome and "profiles" not in welcome
    event = next(e for e in reversed(gadget.events) if e.text == "hi")
    assert event.source.profile is None


DEVICE = "hg-cccccccccccccccc"


def _ops_route(device):
    """A static route a user may have configured before device selection existed."""
    from gateway.profile_routing import parse_profile_routes

    return parse_profile_routes([{"name": "old-route", "platform": "gadget", "profile": "ops", "user_id": device}])


def _routing_rig():
    """The real gateway resolvers around one gadget adapter (upstream's test_multiplex_transport_matrix.py rig)."""
    from gateway.config import GatewayConfig, Platform, PlatformConfig
    from gateway.pairing import PairingStore
    from gateway.run import GatewayRunner
    from hermes_gadget_plugin.adapter import GadgetAdapter

    gadget = Platform("gadget")
    runner = object.__new__(GatewayRunner)
    runner.config = GatewayConfig(multiplex_profiles=True)
    runner.config.platforms = {gadget: PlatformConfig(enabled=True, extra={})}
    runner.config.profile_routes = []  # tests add _ops_route(...) where a device has one
    runner.pairing_store, runner.pairing_stores = PairingStore(profile="default"), {}
    runner._primary_profile_name = "default"
    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra={"port": 0, "host": "127.0.0.1", "profiles": "all"}))
    adapter.gateway_runner = runner
    runner.adapters, runner._profile_adapters = {gadget: adapter}, {"ops": {}}
    return runner, adapter


@pytest.fixture
def routing_rig(hermes_home, tmp_path):
    from hermes_gadget_plugin.store import DeviceStore

    runner, adapter = _routing_rig()
    adapter._store = DeviceStore(tmp_path / "plugin-data")
    return runner, adapter, hermes_home


def _session(profile, capable=True):
    return types.SimpleNamespace(device_id=DEVICE, name="Kitchen", profile=profile,
                                 caps={"profiles": True} if capable else {})


def test_a_device_on_ops_runs_in_the_ops_home_and_replies_through_the_gadget_adapter(routing_rig):
    from gateway.run import _profile_runtime_scope
    from gateway.session_identity import resolve_identity
    from hermes_constants import get_hermes_home

    runner, adapter, home = routing_rig
    source = adapter._source(_session("ops"))
    identity = resolve_identity(source, runner=runner, adapter=adapter)
    assert (identity.transport_profile, identity.runtime_profile) == ("default", "ops")
    assert identity.runtime_home == home / "profiles" / "ops"
    assert identity.authorization_home == home
    assert runner._session_key_for_source(source).startswith("agent:ops:gadget:dm:")
    assert runner._delivery_adapter_for(source) is adapter
    with _profile_runtime_scope(identity.runtime_home):
        assert get_hermes_home() == home / "profiles" / "ops"


@pytest.mark.parametrize("ops", ["served", "parked"])
def test_a_routed_device_keeps_its_route_and_gets_no_switching(routing_rig, ops):
    import asyncio

    from gateway.session_identity import IdentityUnresolved, resolve_identity

    runner, adapter, home = routing_rig
    runner.config.profile_routes = _ops_route(DEVICE)
    if ops == "parked":
        (home / "profiles" / "ops" / "gateway.parked").touch()
        runner._profile_adapters = {}
    for capable in (True, False):  # supporting profiles changes nothing for a routed device
        source = adapter._source(_session("default", capable=capable))
        if ops == "parked":
            assert source.profile_route_rejected is True  # Hermes refuses it, as without this plugin
            with pytest.raises(IdentityUnresolved):
                resolve_identity(source, runner=runner, adapter=adapter)
        else:
            assert resolve_identity(source, runner=runner, adapter=adapter).runtime_profile == "ops"
    session = _session("default")
    assert adapter.profiles(session) == [] and adapter.profiles() != []
    assert asyncio.run(adapter.select_profile(session, "default")) == "routed"
    runner.config.profile_routes = []  # without the route the device picks, default included
    identity = resolve_identity(adapter._source(session), runner=runner, adapter=adapter)
    assert identity.runtime_profile == "default" and identity.runtime_home == home
    assert adapter.profiles(session) != []


def test_owner_check_is_the_adapters_own_check_and_ignores_other_profiles(routing_rig):
    # The default profile's verdict comes from the adapter's own sender check (no private runner call):
    # without a route it is the default profile's answer, and an approval that only ops holds doesn't count.
    from gateway.config import Platform
    from gateway.pairing import PairingStore
    from gateway.run import _profile_runtime_scope

    runner, adapter, home = routing_rig
    ops = PairingStore(profile="ops")
    runner.pairing_stores["ops"] = ops
    with _profile_runtime_scope(home / "profiles" / "ops"):
        ops.approve_code("gadget", ops.generate_code("gadget", DEVICE, "Kitchen"))
    adapter.set_authorization_check(runner._make_adapter_auth_check(Platform("gadget")))
    capable = _session("default")
    assert adapter._owner_trusts(DEVICE) is False
    assert adapter._verdict(capable) is False  # it switches: only the default profile counts
    default = runner.pairing_store
    default.approve_code("gadget", default.generate_code("gadget", DEVICE, "Kitchen"))
    assert adapter._owner_trusts(DEVICE) is True
    assert adapter._verdict(capable) is True


def test_an_unknown_profile_falls_back_to_the_launch_home(routing_rig):
    # Why the plugin refuses unknown profiles itself: Hermes only warns.
    from gateway.session_identity import resolve_identity

    runner, adapter, home = routing_rig
    identity = resolve_identity(adapter._source(_session("nosuch")), runner=runner, adapter=adapter)
    assert identity.runtime_home == home


class _RawDevice:
    """A device speaking the handshake by hand, so a test chooses exactly what it claims in hello."""

    def __init__(self, url, key, caps, profile=None):
        import base64

        from websockets.sync.client import connect
        from hermes_gadget_plugin import protocol

        device = protocol.device_id_for_key(key)
        self.ws = connect(url, subprotocols=["hermes-gadget.v1"])
        self.ws.send(json.dumps({"type": "hello", "proto": 1, "device_id": device, "name": "Kitchen",
                                 "board": "raw", "firmware": "0.1.0", "caps": caps,
                                 **({"profile": profile} if profile else {})}))
        challenge = json.loads(self.ws.recv(timeout=5))
        auth = ({"mac": protocol.auth_mac(key, device, challenge["nonce"])} if challenge["enrolled"]
                else {"key": base64.b64encode(key).decode()})
        self.ws.send(json.dumps({"type": "auth", **auth}))
        self.welcome = json.loads(self.ws.recv(timeout=5))

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.ws.close()


@pytest.fixture
def live_rig(hermes_home, loop_thread, tmp_path, monkeypatch):
    """The adapter listening, behind Hermes's real authorization: its callbacks and per-profile
    pairing stores. ``route()`` adds an old static route sending the device to ops."""
    import gateway.config as gateway_config
    import plugins.plugin_storage as storage
    from gateway.config import Platform
    from gateway.pairing import PairingStore
    from hermes_gadget_plugin import protocol

    monkeypatch.setattr(storage, "plugin_data_dir", lambda name: tmp_path / "plugin-data" / name)
    monkeypatch.setattr(gateway_config, "persist_home_channel", lambda home, **kw: None)
    monkeypatch.delenv("GADGET_HOME_CHANNEL", raising=False)
    key = bytes(range(1, 33))
    device = protocol.device_id_for_key(key)
    runner, adapter = _routing_rig()
    runner.pairing_stores["ops"] = PairingStore(profile="ops")
    adapter.set_authorization_check(runner._make_adapter_auth_check(Platform("gadget")))
    events = []

    async def handler(event):
        events.append(event)

    adapter.set_message_handler(handler)
    assert loop_thread.run(adapter.connect())
    url = f"ws://127.0.0.1:{adapter.hub.bound_port}/gadget"

    def admits(source):
        """Hermes's own ingress decision: canonicalize, then authorize."""
        return runner._canonicalize(source, primary_home=hermes_home) is not None and \
            runner._is_user_authorized_for_source(source)

    def approve(profile="default"):
        from gateway.run import _profile_runtime_scope
        from hermes_cli.profiles import get_profile_dir

        store = runner.pairing_store if profile == "default" else runner.pairing_stores[profile]
        with _profile_runtime_scope(get_profile_dir(profile)):
            store.approve_code("gadget", store.generate_code("gadget", device, "Kitchen"))

    def route():
        runner.config.profile_routes = _ops_route(device)

    yield types.SimpleNamespace(
        runner=runner, adapter=adapter, device=device, events=events, run=loop_thread.run, admits=admits,
        approve=approve, route=route, connect=lambda caps, profile=None: _RawDevice(url, key, caps, profile))
    loop_thread.run(adapter.cancel_background_tasks())
    loop_thread.run(adapter.disconnect())


def _admitted(rig, wait_s=1.5):
    """Events Hermes would run, after giving dispatch a moment to deliver them."""
    import time as _time

    deadline = _time.monotonic() + wait_s
    while _time.monotonic() < deadline:
        if any(rig.admits(e.source) for e in rig.events):
            break
        _time.sleep(0.05)
    return [(e.text, e.source.profile) for e in rig.events if rig.admits(e.source)]


def test_dropping_the_profile_flag_does_not_reach_ops(live_rig):
    rig = live_rig
    rig.approve()
    with rig.connect({"profiles": True}) as device:
        assert device.welcome["paired"] is True
        session = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(session, "ops")) is None
    rig.route()
    rig.runner.pairing_store.revoke("gadget", rig.device)
    with rig.connect({}) as device:  # older firmware, same key, an old ops route
        assert device.welcome["paired"] is False
        session = rig.adapter.hub.get(rig.device)
        rig.run(rig.adapter.on_text(session, "m1", "hi"))
        assert _admitted(rig) == []


def test_a_replaced_connection_cannot_send_voice_after_revocation(live_rig, monkeypatch):
    import asyncio
    import threading

    import hermes_gadget_plugin.adapter as adapter_module

    rig = live_rig
    rig.approve()
    release = threading.Event()

    async def slow_cache(data, ext):
        await asyncio.to_thread(release.wait, 10)  # the upload is still being written
        return "/tmp/utterance.wav"

    monkeypatch.setattr(adapter_module, "cache_audio_from_bytes_async", slow_cache)
    with rig.connect({"profiles": True}):
        old = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(old, "ops")) is None

        async def speak():
            return old.spawn(rig.adapter.on_utterance(old, "u1", b"RIFF", 1.0))

        upload = rig.run(speak())
    rig.runner.pairing_store.revoke("gadget", rig.device)
    with rig.connect({"profiles": True}) as device:  # the device is back, now refused
        assert device.welcome["paired"] is False
        release.set()
        rig.run(asyncio.wait_for(upload, 10))
        assert _admitted(rig) == []


def test_switching_writes_nothing_into_the_target_profile_which_pairs_on_its_own(live_rig):
    # Profiles stay islands: the switch only binds the device to the default profile in the plugin's own
    # store. Ops refuses its turns until ops itself approves the device (hermes -p ops pairing approve ...).
    rig = live_rig
    rig.approve()
    with rig.connect({"profiles": True}):
        session = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(session, "ops")) is None
        assert not rig.runner.pairing_stores["ops"].is_approved("gadget", rig.device)
        assert rig.adapter._store.owner_bound(rig.device)
        rig.run(rig.adapter.on_text(session, "m1", "hi"))
        assert [(e.text, e.source.profile) for e in rig.events if e.text == "hi"] == [("hi", "ops")]
        assert _admitted(rig) == []
        rig.approve("ops")
        assert _admitted(rig) == [("hi", "ops")]


def test_the_target_profiles_pairing_code_reaches_the_device_as_its_command(live_rig):
    from gateway.run_inbound_unauthorized import pairing_code_reply, pairing_profile_arg

    rig = live_rig
    rig.approve()
    with rig.connect({"profiles": True}) as device:
        session = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(session, "ops")) is None
        offer = pairing_code_reply("gadget", "ABCD2345", pairing_profile_arg(rig.runner.pairing_stores["ops"]))
        assert rig.run(rig.adapter.send(rig.device, offer)).success
        seen = []
        while not any(m["type"] == "reply" for m in seen):
            seen.append(json.loads(device.ws.recv(timeout=5)))
        assert {"type": "turn.end", "turn": "", "outcome": "cancelled"}.items() <= next(
            m for m in seen if m["type"] == "turn.end").items()  # no turn runs: the device settles
        reply = next(m for m in seen if m["type"] == "reply")["text"]
        assert "hermes -p ops pairing approve gadget ABCD2345" in reply
        assert session.paired and session.profile == "ops"  # still paired on default, still on ops
        assert rig.adapter._store.pairing_for(rig.device) is None  # default's pairing state untouched


def test_a_switch_that_cannot_bind_the_device_mints_nothing(live_rig, monkeypatch):
    from gateway.pairing import PairingStore

    rig = live_rig
    rig.approve()

    def fail(device_id):
        raise OSError("disk full")

    monkeypatch.setattr(rig.adapter._store, "bind_to_owner", fail)
    with rig.connect({"profiles": True}):
        session = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(session, "ops")) == "unpaired"
        assert session.profile == "default"
    assert not PairingStore(profile="ops").is_approved("gadget", rig.device)


def test_forget_from_a_stale_store_keeps_the_device_bound(live_rig):
    from hermes_gadget_plugin.store import DeviceStore

    rig = live_rig
    rig.approve()
    directory = rig.adapter._store.path.parent
    with rig.connect({"profiles": True}):
        session = rig.adapter.hub.get(rig.device)
        cli = DeviceStore(directory)  # 'hermes gadget forget' in another process, loaded before the switch
        assert rig.run(rig.adapter.select_profile(session, "ops")) is None
    assert cli.forget(rig.device)  # rewrites devices.json from its older snapshot
    rig.adapter._store = DeviceStore(directory)  # as after a gateway restart
    rig.route()
    rig.runner.pairing_store.revoke("gadget", rig.device)
    with rig.connect({}) as legacy:  # re-enrolls the same key, no profile flag, old ops route
        assert legacy.welcome["paired"] is False
        rig.run(rig.adapter.on_text(rig.adapter.hub.get(rig.device), "m1", "after-forget"))
        assert _admitted(rig) == []


def test_an_upgraded_routed_device_keeps_its_route_and_pairing(live_rig):
    rig = live_rig
    rig.route()
    rig.approve("ops")  # approved where its route sends it, never on default
    with rig.connect({"profiles": True}, profile="default") as device:
        assert device.welcome["paired"] is True
        assert "profile" not in device.welcome and "profiles" not in device.welcome
        session = rig.adapter.hub.get(rig.device)
        rig.run(rig.adapter.on_text(session, "m1", "hi"))
        assert _admitted(rig) == [("hi", "ops")]
        device.ws.send(json.dumps({"type": "profile.select", "profile": "default"}))
        while (reply := json.loads(device.ws.recv(timeout=5)))["type"] != "profile":
            pass
        assert reply == {"type": "profile", "profile": "default", "error": "routed"}
    assert not rig.adapter._store.owner_bound(rig.device)
