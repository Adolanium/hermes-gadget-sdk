"""Profile rosters, routing and binding to the default profile, on real Hermes classes."""
from __future__ import annotations

import json
import time
import types
import pytest
from conftest import requires_hermes, requires_sim

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


def _dashboard_labels(device, seconds):
    """The session labels of each dashboard the device gets within ``seconds``."""
    import time

    seen, end = [], time.monotonic() + seconds
    while (left := end - time.monotonic()) > 0:
        try:
            msg = json.loads(device.ws.recv(timeout=left))
        except TimeoutError:
            break
        if msg.get("type") == "dashboard":
            seen.append([row["label"] for row in msg["sessions"]])
    return seen


def _profile_board(monkeypatch, profile):
    """A session board registered the way ``profile``'s own copy of the plugin registers it."""
    from hermes_gadget_plugin import dashboard, runtime

    monkeypatch.setitem(runtime._boards, profile, None)  # so the registration is undone after the test
    board = dashboard.SessionBoard()
    board.register(types.SimpleNamespace(profile_name=profile, register_hook=lambda name, callback: None))
    return board


def test_only_a_switching_device_sees_the_offered_profiles_sessions(hermes_home, gadget, monkeypatch):
    import os
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)  # only a wake sends a change below
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})  # the default profile's board: no sessions
    ops = _profile_board(monkeypatch, "ops")
    old = _profile_board(monkeypatch, "old")  # parked, so not served and never offered
    old.pre_llm_call(session_id="s_old1", platform="cli")
    ops.pre_llm_call(session_id="s_ops1", platform="telegram")
    caps = {"dashboard": {"v": dashboard.VERSION}}
    keys = os.urandom(32), os.urandom(32)
    gadget.authorized.update(protocol.device_id_for_key(k) for k in keys)
    with _RawDevice(gadget.url, keys[0], {**caps, "profiles": True}) as switching, \
            _RawDevice(gadget.url, keys[1], caps) as old_firmware:
        assert _dashboard_labels(switching, 2.5)[-1] == ["telegram ops1"]  # outlasts the poll begun before
        ops.pre_approval_request(session_id="s_ops2")  # wakes the dashboard as a session of its own does
        assert _dashboard_labels(switching, 1.5)[-1] == ["session ops2", "telegram ops1"]
        assert _dashboard_labels(old_firmware, 0.5) == [[]]  # exactly today's: the default profile's only


WITH_OPS, WITHOUT_OPS = [{"id": "default"}, {"id": "ops"}], [{"id": "default"}]



@pytest.mark.parametrize("rosters, rows, heard", [
    ([WITH_OPS, WITHOUT_OPS], [], False),  # ops is parked: an older push cannot bring it back
    ([WITHOUT_OPS, WITH_OPS], ["telegram ops1"], True),  # ops is unparked: an older roster cannot drop it
], ids=["ops leaves the offer", "ops joins the offer"])
def test_the_newer_of_two_pushes_decides_the_dashboard(hermes_home, gadget, monkeypatch, rosters, rows, heard):
    import asyncio
    import os
    import threading
    import time
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)  # only the two pushes below read the roster
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    ops = _profile_board(monkeypatch, "ops")
    ops.pre_llm_call(session_id="s_ops1", platform="telegram")
    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    with _RawDevice(gadget.url, key, {"dashboard": {"v": dashboard.VERSION}, "profiles": True}) as device:
        assert _dashboard_labels(device, 2.5)[-1] == ["telegram ops1"]  # outlasts the poll begun before
        releases, reads = [threading.Event(), threading.Event()], []

        def profiles(session=None):
            n = len(reads)
            reads.append(n)
            releases[n].wait(10)
            return rosters[n]

        monkeypatch.setattr(gadget.adapter, "profiles", profiles)
        older = asyncio.run_coroutine_threadsafe(gadget.adapter._push_dashboard(), gadget.loop)
        while not reads:
            time.sleep(0.01)
        newer = asyncio.run_coroutine_threadsafe(gadget.adapter._push_dashboard(), gadget.loop)
        time.sleep(0.3)  # without one push at a time, the newer roster read starts now
        releases[1].set()  # the newer read may finish first
        time.sleep(0.3)
        releases[0].set()
        older.result(5)
        newer.result(5)
        assert _dashboard_labels(device, 0.5)[-1] == rows
        assert ops._listeners == ([gadget.adapter._board_changed] if heard else [])



def test_a_push_held_up_by_another_device_does_not_send_its_older_roster(hermes_home, gadget, monkeypatch):
    import asyncio
    import os
    import threading
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    _profile_board(monkeypatch, "ops").pre_llm_call(session_id="s_ops1", platform="telegram")
    caps = {"dashboard": {"v": dashboard.VERSION}, "profiles": True}
    keys = os.urandom(32), os.urandom(32)
    gadget.authorized.update(protocol.device_id_for_key(k) for k in keys)
    stuck, entered = asyncio.Event(), threading.Event()

    async def send_dashboard(_msg):
        entered.set()
        await stuck.wait()
        return True

    with _RawDevice(gadget.url, keys[0], caps), _RawDevice(gadget.url, keys[1], caps) as b:
        assert _dashboard_labels(b, 2.5)[-1] == ["telegram ops1"]  # outlasts the poll begun before
        a, b_session = (gadget.adapter.hub.get(protocol.device_id_for_key(k)) for k in keys)
        monkeypatch.setattr(a, "send_dashboard", send_dashboard)
        older = asyncio.run_coroutine_threadsafe(gadget.adapter._push_dashboard(), gadget.loop)  # A, then B
        assert entered.wait(5)  # it has read the roster with ops and waits on A
        monkeypatch.setattr(gadget.adapter, "_offer_all", False)
        monkeypatch.setattr(gadget.adapter, "_offered", set())  # ops is taken off the offer
        try:
            gadget.run(gadget.adapter._push_dashboard(b_session))
            assert _dashboard_labels(b, 0.5) == [[]]
        finally:
            gadget.loop.call_soon_threadsafe(stuck.set)
            older.result(5)
        assert _dashboard_labels(b, 0.5) == []  # the older push skips B: its roster still had ops



@pytest.mark.parametrize("switching", [False, True], ids=["switching off", "switching on"])
def test_a_device_stuck_in_a_dashboard_send_does_not_hold_up_another(hermes_home, gadget, monkeypatch, switching):
    import asyncio
    import os
    import time
    from hermes_gadget_plugin import dashboard, protocol

    if not switching:
        monkeypatch.setattr(gadget.adapter, "_offer_all", False)
        monkeypatch.setattr(gadget.adapter, "_offered", None)
    caps = {"dashboard": {"v": dashboard.VERSION}, "profiles": True}
    keys = os.urandom(32), os.urandom(32)
    gadget.authorized.update(protocol.device_id_for_key(k) for k in keys)
    stuck = asyncio.Event()  # never set before the end: A's socket accepts nothing

    async def send_dashboard(_msg):
        await stuck.wait()
        return True

    with _RawDevice(gadget.url, keys[0], caps):
        a = gadget.adapter.hub.get(protocol.device_id_for_key(keys[0]))
        monkeypatch.setattr(a, "send_dashboard", send_dashboard)
        pushing = asyncio.run_coroutine_threadsafe(gadget.adapter._push_dashboard(), gadget.loop)
        try:
            with _RawDevice(gadget.url, keys[1], caps) as b:
                b.ws.send(json.dumps({"type": "text", "id": "m1", "text": "from b"}))
                end = time.monotonic() + 3
                while not any(e.text == "from b" for e in gadget.events) and time.monotonic() < end:
                    time.sleep(0.02)
                assert any(e.text == "from b" for e in gadget.events)  # B's input runs while A's send waits
        finally:
            gadget.loop.call_soon_threadsafe(stuck.set)
            pushing.exception(5)  # the push may then find a closed socket; only its end matters



def test_a_device_that_leaves_takes_its_push_order_with_it(hermes_home, gadget):
    import os
    import time
    from hermes_gadget_plugin import dashboard, protocol

    keys = os.urandom(32), os.urandom(32)
    gone, stays = (protocol.device_id_for_key(k) for k in keys)
    gadget.authorized.update((gone, stays))
    caps = {"dashboard": {"v": dashboard.VERSION}, "profiles": True}
    with _RawDevice(gadget.url, keys[1], caps):
        with _RawDevice(gadget.url, keys[0], caps):
            end = time.monotonic() + 5
            while not {gone, stays} <= set(gadget.adapter._pushed_seq) and time.monotonic() < end:
                time.sleep(0.02)
            assert {gone, stays} <= set(gadget.adapter._pushed_seq)
        end = time.monotonic() + 5
        while gone in gadget.adapter._pushed_seq and time.monotonic() < end:
            time.sleep(0.02)
        assert gone not in gadget.adapter._pushed_seq and stays in gadget.adapter._pushed_seq
    gadget.adapter._pushed_seq["hg-0000000000000000"] = 1  # as a push still finishing at shutdown leaves
    gadget.run(gadget.adapter.disconnect())
    assert gadget.adapter._pushed_seq == {}



def test_a_push_queued_while_a_device_leaves_does_not_remember_it(hermes_home, gadget):
    import asyncio
    import os
    import time
    from hermes_gadget_plugin import dashboard, protocol

    keys = os.urandom(32), os.urandom(32)
    stays, gone = (protocol.device_id_for_key(k) for k in keys)
    gadget.authorized.update((stays, gone))
    caps = {"dashboard": {"v": dashboard.VERSION}, "profiles": True}
    adapter = gadget.adapter

    def until(check):
        end = time.monotonic() + 5
        while not check() and time.monotonic() < end:
            time.sleep(0.02)
        return check()

    with _RawDevice(gadget.url, keys[0], caps):
        with _RawDevice(gadget.url, keys[1], caps):
            assert until(lambda: {stays, gone} <= set(adapter._pushed_seq))
            gadget.run(adapter._roster_lock.acquire())  # another push is reading the roster
            queued = asyncio.run_coroutine_threadsafe(adapter._push_dashboard(), gadget.loop)  # both devices
            time.sleep(0.1)
        assert until(lambda: gone not in adapter.hub.sessions and gone not in adapter._pushed_seq)
        gadget.loop.call_soon_threadsafe(adapter._roster_lock.release)
        queued.result(5)
        assert gone not in adapter._pushed_seq and stays in adapter._pushed_seq


def test_a_device_that_cannot_switch_leaves_the_switching_ones_listening(hermes_home, gadget, monkeypatch):
    import os
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)  # only a wake can carry the change below
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    ops = _profile_board(monkeypatch, "ops")
    ops.on_session_start(session_id="s_ops1", platform="telegram")
    caps = {"dashboard": {"v": dashboard.VERSION}}
    keys = os.urandom(32), os.urandom(32)
    gadget.authorized.update(protocol.device_id_for_key(k) for k in keys)
    with _RawDevice(gadget.url, keys[0], {**caps, "profiles": True}) as switching:
        assert _dashboard_labels(switching, 2.5)[-1] == ["telegram ops1"]  # outlasts the poll begun before
        with _RawDevice(gadget.url, keys[1], caps) as plain:
            assert _dashboard_labels(plain, 1.0) == [[]]  # its own first dashboard has been pushed
            ops.pre_llm_call(session_id="s_ops1", platform="telegram")
            msgs = []
            end = time.monotonic() + 1.5
            while time.monotonic() < end and not msgs:
                try:
                    msg = json.loads(switching.ws.recv(timeout=end - time.monotonic()))
                except TimeoutError:
                    break
                if msg.get("type") == "dashboard":
                    msgs.append(msg)
            assert [row["state"] for row in msgs[-1]["sessions"]] == ["busy"]  # the ops change woke it


def test_a_device_that_cannot_switch_gets_the_rows_from_after_the_roster_wait(hermes_home, gadget, monkeypatch):
    import os
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)  # only a wake sends a dashboard below
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    _profile_board(monkeypatch, "ops")
    caps = {"dashboard": {"v": dashboard.VERSION}}
    keys = os.urandom(32), os.urandom(32)
    gadget.authorized.update(protocol.device_id_for_key(k) for k in keys)
    with _RawDevice(gadget.url, keys[0], {**caps, "profiles": True}) as switching, \
            _RawDevice(gadget.url, keys[1], caps) as plain:
        _dashboard_labels(switching, 2.5), _dashboard_labels(plain, 0.5)  # their first dashboards
        reading, release = _paused_roster(monkeypatch, gadget.adapter)
        dashboard.BOARD.pre_llm_call(session_id="s_aaa1", platform="cli")  # wakes an update to both
        assert reading.wait(5)  # that update waits for the roster
        dashboard.BOARD.pre_llm_call(session_id="s_bbb2", platform="cli")  # changes during the wait
        release.set()
        # One dashboard, with both sessions: no stale one taken before the wait goes out first.
        assert _dashboard_labels(plain, 1.5) == [["cli bbb2", "cli aaa1"]]


@pytest.mark.parametrize("late_roster", [False, True], ids=["last device leaves", "a roster read ends after"])
def test_the_last_switching_device_leaving_drops_the_other_profiles_listeners(hermes_home, gadget, monkeypatch,
                                                                              late_roster):
    import asyncio
    import os
    from hermes_gadget_plugin import dashboard, protocol

    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    caps = {"dashboard": {"v": dashboard.VERSION}, "profiles": True}
    with _RawDevice(gadget.url, key, caps) as device:
        if late_roster:
            _dashboard_labels(device, 0.5)  # its first dashboard, before ops's copy of the plugin loads
            ops = _profile_board(monkeypatch, "ops")
            reading, release = _paused_roster(monkeypatch, gadget.adapter)
            broadcast = asyncio.run_coroutine_threadsafe(gadget.adapter._push_dashboard(), gadget.loop)
            assert reading.wait(5)
        else:
            ops = _profile_board(monkeypatch, "ops")
            end = time.monotonic() + 5
            while not ops._listeners and time.monotonic() < end:
                time.sleep(0.02)
            assert ops._listeners
    if late_roster:
        release.set()  # the read ends after the device has gone, and subscribes to ops
        broadcast.exception(5)
        assert ops._listeners
    end = time.monotonic() + 3  # a poll or two
    while ops._listeners and time.monotonic() < end:
        time.sleep(0.05)
    assert ops._listeners == []  # nothing left to wake for


def _sessions_mine_and_ops(monkeypatch):
    from hermes_gadget_plugin import dashboard

    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    dashboard.BOARD.pre_llm_call(session_id="s_mine", platform="cli")  # the default profile's own
    _profile_board(monkeypatch, "ops").pre_llm_call(session_id="s_ops1", platform="telegram")


@pytest.mark.parametrize("offered", [None, set()], ids=["switching off", "ops served but not offered"])
def test_without_the_offer_a_device_gets_only_the_default_profiles_sessions(hermes_home, gadget, monkeypatch,
                                                                            offered):
    import os
    from hermes_gadget_plugin import dashboard, protocol

    monkeypatch.setattr(gadget.adapter, "_offer_all", False)
    monkeypatch.setattr(gadget.adapter, "_offered", offered)  # no `profiles` key, or `profiles: []`
    _sessions_mine_and_ops(monkeypatch)
    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    with _RawDevice(gadget.url, key, {"dashboard": {"v": dashboard.VERSION}, "profiles": True}) as device:
        assert _dashboard_labels(device, 2.5) == [["cli mine"]]  # exactly today's


def test_a_routed_device_gets_only_the_default_profiles_sessions(live_rig, monkeypatch):
    from hermes_gadget_plugin import dashboard

    _sessions_mine_and_ops(monkeypatch)
    live_rig.route()  # to ops, with switching on for every other device
    live_rig.approve("ops")
    with live_rig.connect({"dashboard": {"v": dashboard.VERSION}, "profiles": True}, profile="default") as device:
        assert device.welcome["paired"] is True
        assert _dashboard_labels(device, 2.5) == [["cli mine"]]  # exactly today's


def _paused_roster(monkeypatch, adapter):
    """Hold the dashboard's roster lookup (``profiles()`` with no session) until ``release`` is set."""
    import threading

    reading, release, roster = threading.Event(), threading.Event(), adapter.profiles

    def profiles(session=None):
        if session is None:
            reading.set()
            release.wait(10)
        return roster(session)

    monkeypatch.setattr(adapter, "profiles", profiles)
    return reading, release


def test_a_device_revoked_while_the_roster_is_read_gets_no_dashboard(hermes_home, gadget, monkeypatch):
    import os
    from hermes_gadget_plugin import dashboard, protocol

    _profile_board(monkeypatch, "ops").pre_llm_call(session_id="s_ops1", platform="telegram")
    reading, release = _paused_roster(monkeypatch, gadget.adapter)
    key = os.urandom(32)
    device_id = protocol.device_id_for_key(key)
    gadget.authorized.add(device_id)
    with _RawDevice(gadget.url, key, {"dashboard": {"v": dashboard.VERSION}, "profiles": True}) as device:
        assert reading.wait(5)  # the first dashboard is waiting for the roster
        gadget.authorized.discard(device_id)  # revoked in the default profile
        while json.loads(device.ws.recv(timeout=10)).get("type") != "unpaired":
            pass
        release.set()
        assert _dashboard_labels(device, 1.0) == []


def test_a_roster_read_still_running_at_disconnect_leaves_no_listener(hermes_home, gadget, monkeypatch):
    import asyncio
    import os
    import time
    from hermes_gadget_plugin import dashboard, protocol

    ops = _profile_board(monkeypatch, "ops")
    reading, release = _paused_roster(monkeypatch, gadget.adapter)
    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    with _RawDevice(gadget.url, key, {"dashboard": {"v": dashboard.VERSION}, "profiles": True}):
        assert reading.wait(5)  # the first dashboard is waiting for the roster
        stopping = asyncio.run_coroutine_threadsafe(gadget.adapter.disconnect(), gadget.loop)
        time.sleep(0.3)  # disconnect has dropped its listeners and waits for the connection to end
        release.set()
        stopping.result(10)
        time.sleep(0.3)
    assert ops._listeners == []  # nothing keeps calling the stopped adapter


def test_a_board_no_longer_offered_or_replaced_loses_its_listener(hermes_home, gadget, monkeypatch):
    import os
    import time
    from hermes_gadget_plugin import adapter as adapter_module, dashboard, protocol

    monkeypatch.setattr(adapter_module, "DASHBOARD_POLL_S", 60)  # only a wake sends a dashboard below
    monkeypatch.setattr(dashboard.BOARD, "_sessions", {})
    ops = _profile_board(monkeypatch, "ops")
    ops.pre_llm_call(session_id="s_ops1", platform="telegram")
    key = os.urandom(32)
    gadget.authorized.add(protocol.device_id_for_key(key))
    with _RawDevice(gadget.url, key, {"dashboard": {"v": dashboard.VERSION}, "profiles": True}) as device:
        assert _dashboard_labels(device, 2.5)[-1] == ["telegram ops1"]
        assert len(ops._listeners) == 1
        reloaded = _profile_board(monkeypatch, "ops")  # a reloaded copy of ops's plugin registers anew
        dashboard.BOARD.pre_llm_call(session_id="s_mine", platform="cli")  # wakes the dashboard
        _dashboard_labels(device, 1.0)
        assert ops._listeners == [] and len(reloaded._listeners) == 1
        monkeypatch.setattr(gadget.adapter, "_offer_all", False)
        monkeypatch.setattr(gadget.adapter, "_offered", {"default"})  # ops is taken off the list
        dashboard.BOARD.pre_llm_call(session_id="s_min2", platform="cli")
        assert _dashboard_labels(device, 1.0)[-1] == ["cli min2", "cli mine"]
        time.sleep(0.1)
        assert reloaded._listeners == []  # nothing wakes the dashboard for a profile it no longer shows


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


def test_a_routed_device_gets_the_face_of_the_profile_its_route_names(routing_rig):
    import asyncio

    from hermes_gadget_plugin.face import FACE_BYTES, FACE_FILE

    from gateway.config import Platform

    runner, adapter, home = routing_rig
    adapter.set_authorization_check(runner._make_adapter_auth_check(Platform("gadget")))  # as the gateway installs it
    (home / FACE_FILE).write_bytes(bytes([0xAA]) * FACE_BYTES)
    (home / "profiles" / "ops" / FACE_FILE).write_bytes(bytes([0x55]) * FACE_BYTES)
    session = _session("default")
    assert asyncio.run(adapter.face(session)).load() == bytes([0xAA]) * FACE_BYTES  # no route: the default profile
    runner.config.profile_routes = _ops_route(DEVICE)
    assert asyncio.run(adapter.face(session)) is None  # ops hasn't approved the device yet
    from gateway.pairing import PairingStore
    from gateway.run import _profile_runtime_scope

    ops = runner.pairing_stores["ops"] = PairingStore(profile="ops")
    with _profile_runtime_scope(home / "profiles" / "ops"):
        ops.approve_code("gadget", ops.generate_code("gadget", DEVICE, "Kitchen"))
    assert asyncio.run(adapter.face(session)).load() == bytes([0x55]) * FACE_BYTES
    (home / "profiles" / "ops" / "gateway.parked").touch()
    runner._profile_adapters = {}
    assert asyncio.run(adapter.face(session)) is None  # Hermes refuses the route: no profile's face


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


def test_owner_check_is_default_scoped_and_ignores_other_profiles(routing_rig):
    # The default profile's verdict is the default profile's own answer: an approval that only ops holds
    # doesn't count. A static route would make ops answer instead, so a routed device fails closed.
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
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(adapter, "gateway_runner", None)  # unrouted: the public sender check answers alone
        assert adapter._owner_trusts(DEVICE) is True
    runner.config.profile_routes = _ops_route(DEVICE)  # the route would make ops answer for the default profile
    assert adapter._owner_trusts(DEVICE) is False  # so it fails closed, though the default profile approves it
    runner.config.profile_routes = []
    assert adapter._owner_trusts(DEVICE) is True
    default.revoke("gadget", DEVICE)
    assert adapter._owner_trusts(DEVICE) is False


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

    def store(profile):
        return runner.pairing_store if profile == "default" else runner.pairing_stores[profile]

    def approve(profile="default", device_id=device):
        from gateway.run import _profile_runtime_scope
        from hermes_cli.profiles import get_profile_dir

        with _profile_runtime_scope(get_profile_dir(profile)):
            store(profile).approve_code("gadget", store(profile).generate_code("gadget", device_id, "Kitchen"))

    def revoke(profile, device_id=device):
        from gateway.run import _profile_runtime_scope
        from hermes_cli.profiles import get_profile_dir

        with _profile_runtime_scope(get_profile_dir(profile)):
            assert store(profile).revoke("gadget", device_id)

    def route():
        runner.config.profile_routes = _ops_route(device)

    yield types.SimpleNamespace(
        runner=runner, adapter=adapter, device=device, events=events, run=loop_thread.run, admits=admits,
        approve=approve, revoke=revoke, route=route, url=url,
        connect=lambda caps, profile=None: _RawDevice(url, key, caps, profile))
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
    # The device switched once, so it answers to the default profile for good. Ops's own approval and an
    # old static route to ops must not stand in for the default approval once that one is revoked.
    rig = live_rig
    rig.approve()
    with rig.connect({"profiles": True}) as device:
        assert device.welcome["paired"] is True
        session = rig.adapter.hub.get(rig.device)
        assert rig.run(rig.adapter.select_profile(session, "ops")) is None
    rig.approve("ops")
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


def _faces_on_ops(rig, hermes_home, make_sim):
    """A native device trusted by the default profile, holding default's face, then switched to ops;
    ops hasn't approved it. Returns the device, a function giving the face it holds, and the hashes."""
    import struct

    from hermes_gadget_plugin.face import FACE_BYTES, FACE_FILE, ProfileFace

    anchors = struct.pack("<10h", 607, 293, 262, 620, 801, 107, 396, 508, 586, 649)
    face = {"default": bytes([0xAA]) * (FACE_BYTES - 20) + anchors, "ops": bytes([0x55]) * (FACE_BYTES - 20) + anchors}
    (hermes_home / FACE_FILE).write_bytes(face["default"])
    (hermes_home / "profiles" / "ops" / FACE_FILE).write_bytes(face["ops"])
    digest = {p: ProfileFace.of(data).hash for p, data in face.items()}
    sim = make_sim(rig.url)

    def held():
        """The face the device holds, as the hello of a reconnect names it."""
        sim.console("reconnect")
        assert sim.wait_screen("connecting", timeout=5)
        assert sim.wait_screen("ready", timeout=10)
        return [m for m in sim.sent if m["type"] == "hello"][-1].get("face")

    assert sim.wait_screen("pairing", "ready", timeout=10)
    rig.approve("default", sim.status()["device_id"])
    assert sim.wait_screen("ready", timeout=10)
    assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == digest["default"], timeout=5)
    session = rig.adapter.hub.get(sim.status()["device_id"])
    assert rig.run(rig.adapter.select_profile(session, "ops")) is None
    rig.run(session.send_profile())
    sim.run_for(3)  # longer than the pairing check
    return sim, held, digest


@requires_sim
def test_a_switched_device_shows_its_profiles_face_without_that_profiles_approval(live_rig, hermes_home, make_sim):
    # The default profile trusts the device, which is the gate switching uses; ops's own approval isn't needed.
    sim, held, digest = _faces_on_ops(live_rig, hermes_home, make_sim)
    assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == digest["ops"], timeout=8)
    assert held() == digest["ops"]


@requires_sim
def test_the_picked_profiles_own_revocation_keeps_its_face(live_rig, hermes_home, make_sim):
    sim, held, digest = _faces_on_ops(live_rig, hermes_home, make_sim)
    device_id = sim.status()["device_id"]
    live_rig.approve("ops", device_id)
    live_rig.revoke("ops", device_id)
    sim.run_for(3)  # past the next pairing check
    assert held() == digest["ops"]


@requires_sim
def test_the_default_profiles_revocation_clears_the_face(live_rig, hermes_home, make_sim):
    sim, held, digest = _faces_on_ops(live_rig, hermes_home, make_sim)
    assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == digest["ops"], timeout=8)
    live_rig.revoke("default", sim.status()["device_id"])
    assert sim.wait_screen("pairing", timeout=10)  # unpaired: the device drops its face
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_screen("pairing", timeout=10)
    assert "face" not in [m for m in sim.sent if m["type"] == "hello"][-1]


@requires_sim
def test_the_default_profiles_face_needs_only_its_own_approval(live_rig, hermes_home, make_sim):
    sim, held, digest = _faces_on_ops(live_rig, hermes_home, make_sim)
    session = live_rig.adapter.hub.get(sim.status()["device_id"])
    assert live_rig.run(live_rig.adapter.select_profile(session, "default")) is None
    before = len(sim.received)
    live_rig.run(session.send_profile())
    assert sim.wait_for(lambda: any(m["type"] == "face" and m.get("hash") == digest["default"]
                                    for m in sim.received[before:]), timeout=5)
    assert held() == digest["default"]


def _revoked_during_the_welcome_lookup(rig, hermes_home, make_sim, monkeypatch, held_before):
    """A device on ops reconnects; while the welcome's lookup reads ops's face file, the default profile
    revokes the device and the pairing check reacts. Returns the face the device then holds."""
    import asyncio
    import struct
    import threading

    from hermes_gadget_plugin import face as faces

    anchors = struct.pack("<10h", 607, 293, 262, 620, 801, 107, 396, 508, 586, 649)
    ops_face = bytes([0x55]) * (faces.FACE_BYTES - 20) + anchors
    ops_file = hermes_home / "profiles" / "ops" / faces.FACE_FILE
    sim = make_sim(rig.url)

    def held():
        """The face the device holds, as the hello of a reconnect names it."""
        sim.console("reconnect")
        assert sim.wait_screen("connecting", timeout=5)
        assert sim.wait_screen("ready", timeout=10)
        return [m for m in sim.sent if m["type"] == "hello"][-1].get("face")

    assert sim.wait_screen("pairing", "ready", timeout=10)
    device_id = sim.status()["device_id"]
    rig.approve("default", device_id)
    rig.approve("ops", device_id)
    assert sim.wait_screen("ready", timeout=10)
    if held_before:
        ops_file.write_bytes(ops_face)
    session = rig.adapter.hub.get(device_id)
    assert rig.run(rig.adapter.select_profile(session, "ops")) is None
    rig.run(session.send_profile())
    if held_before:
        assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("profile") == "ops", timeout=5)
        assert held() == faces.ProfileFace.of(ops_face).hash
    else:
        sim.run_for(0.5)
        assert held() is None
        ops_file.write_bytes(ops_face)

    entered, release = threading.Event(), threading.Event()
    read = faces.for_profile

    def held_read(home, convert=True):  # the welcome's lookup of ops's face, held until released
        if home == ops_file.parent and not entered.is_set():
            entered.set()
            release.wait(10)
        return read(home, convert)
    monkeypatch.setattr(faces, "for_profile", held_read)

    welcomes = len([m for m in sim.received if m["type"] == "welcome"])
    sim.console("reconnect")
    assert sim.wait_for(entered.is_set, timeout=10)
    rig.revoke("default", device_id)
    current = rig.adapter.hub.get(device_id)

    async def pairing_check_reacts():  # what the 2 s pairing check does when it sees the revocation
        await asyncio.gather(current.update_face(), return_exceptions=True)
    rig.run(pairing_check_reacts())
    release.set()
    assert sim.wait_for(lambda: len([m for m in sim.received if m["type"] == "welcome"]) > welcomes, timeout=10)
    sim.run_for(3)  # past the next pairing check
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_screen("pairing", "ready", timeout=10)
    return [m for m in sim.sent if m["type"] == "hello"][-1].get("face")


@requires_sim
def test_a_revocation_during_the_welcome_lookup_installs_no_face(live_rig, hermes_home, make_sim, monkeypatch):
    assert _revoked_during_the_welcome_lookup(live_rig, hermes_home, make_sim, monkeypatch, held_before=False) is None


@requires_sim
def test_a_revocation_during_the_welcome_lookup_drops_the_held_face(live_rig, hermes_home, make_sim, monkeypatch):
    assert _revoked_during_the_welcome_lookup(live_rig, hermes_home, make_sim, monkeypatch, held_before=True) is None


@requires_sim
def test_a_switched_device_a_route_later_refuses_drops_its_face(live_rig, hermes_home, make_sim):
    """A device that switched answers to the default profile for good; a route added later makes the plugin
    refuse it, even though the routed profile approved it. It must not keep, or get, any profile's face."""
    sim, held, digest = _faces_on_ops(live_rig, hermes_home, make_sim)
    device_id = sim.status()["device_id"]
    live_rig.approve("ops", device_id)
    assert sim.wait_for(lambda: (sim.last_received("face") or {}).get("hash") == digest["ops"], timeout=8)
    assert held() == digest["ops"]

    live_rig.runner.config.profile_routes = _ops_route(device_id)
    assert sim.wait_for(lambda: sim.status()["paired"] is False, timeout=8)  # refused at the next pairing check
    sent = len(sim.received)
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_for(lambda: len([m for m in sim.received[sent:] if m["type"] == "welcome"]) == 1, timeout=10)
    assert [m for m in sim.sent if m["type"] == "hello"][-1].get("face") is None  # the face went with the refusal
    assert sim.last_received("welcome")["paired"] is False
    sim.run_for(3)  # past the next pairing check
    assert not any("bits" in m for m in sim.received[sent:] if m["type"] == "face")
