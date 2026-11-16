"""The dashboard: session state from Hermes's hooks, status tiles, and how the hub delivers
snapshots (only changes; only the latest while the screen is dark). docs/protocol.md#dashboard
"""

from __future__ import annotations

import json
import time

import pytest

from hermes_gadget_plugin import dashboard
from hermes_gadget_plugin.dashboard import SessionBoard, TileStore
from test_hub_screen_sleep import Device, _wait, types


class Clock:
    def __init__(self, t: float = 1000.0):
        self.t = t

    def __call__(self) -> float:
        return self.t


def states(board: SessionBoard) -> dict[str, str]:
    return {row["label"]: row["state"] for row in board.sessions()}


# -- session state from hooks --------------------------------------------------------------

def test_a_turn_is_busy_until_it_ends_and_an_approval_waits_in_between():
    board = SessionBoard()
    board.on_session_start(session_id="20261018_abcd", platform="telegram")
    assert states(board) == {"telegram abcd": "idle"}
    board.pre_llm_call(session_id="20261018_abcd", platform="telegram", user_message="hi")
    assert states(board) == {"telegram abcd": "busy"}
    board.pre_approval_request(session_id="20261018_abcd", command="rm -rf build", surface="gateway")
    assert states(board) == {"telegram abcd": "waiting"}
    board.pre_approval_request(session_id="20261018_abcd", command="rm x", coalesced=True)  # a second wait
    board.post_approval_response(session_id="20261018_abcd", choice="once")
    assert states(board) == {"telegram abcd": "waiting"}  # one is still open
    board.post_approval_response(session_id="20261018_abcd", choice="deny")
    assert states(board) == {"telegram abcd": "busy"}
    board.on_session_end(session_id="20261018_abcd", completed=True, platform="telegram")
    assert states(board) == {"telegram abcd": "idle"}


def test_an_interrupted_turn_with_an_open_approval_ends_idle():
    board = SessionBoard()
    board.pre_llm_call(session_id="s-1", platform="cli")
    board.pre_approval_request(session_id="s-1")
    board.on_session_end(session_id="s-1", interrupted=True)
    assert states(board) == {"cli s-1": "idle"}
    board.pre_llm_call(session_id="s-1", platform="cli")
    board.post_approval_response(session_id="s-1", choice="timeout")  # late: nothing is waiting any more
    assert states(board) == {"cli s-1": "busy"}


def test_subagents_finalized_and_reset_sessions_leave_the_board():
    board = SessionBoard()
    board.pre_llm_call(session_id="child", platform="cli", parent_session_id="parent")
    assert board.sessions() == []
    board.pre_llm_call(session_id="old1", platform="discord")
    board.on_session_reset(session_id="new2", old_session_id="old1", reason="new_session")
    assert board.sessions() == []
    board.pre_llm_call(session_id="gone", platform="cli")
    board.on_session_finalize(session_id="gone")
    assert board.sessions() == []
    board.pre_approval_request()  # no session id: nothing to show
    assert board.sessions() == []


def test_order_is_waiting_busy_idle_and_old_idle_sessions_expire():
    clock = Clock()
    board = SessionBoard(clock=clock)
    for sid, platform in (("aaaa", "cli"), ("bbbb", "telegram"), ("cccc", "gadget")):
        board.on_session_start(session_id=sid, platform=platform)
        clock.t += 1
    board.pre_llm_call(session_id="aaaa")
    board.pre_llm_call(session_id="bbbb")
    board.pre_approval_request(session_id="bbbb")
    assert [r["state"] for r in board.sessions()] == ["waiting", "busy", "idle"]
    assert board.sessions()[0] == {"id": "bbbb", "label": "telegram bbbb", "state": "waiting"}
    clock.t += dashboard.IDLE_KEEP_S + 1
    assert [r["label"] for r in board.sessions()] == ["telegram bbbb", "cli aaaa"]  # the idle one left
    clock.t += dashboard.ACTIVE_KEEP_S
    assert board.sessions() == []  # turns that never reported their end don't stay forever


def test_other_profiles_boards_merge_in_the_same_order_up_to_the_limit():
    clock = Clock()
    mine, ops = SessionBoard(clock=clock), SessionBoard(clock=clock)
    mine.on_session_start(session_id="mine", platform="cli")
    clock.t += 1
    ops.pre_llm_call(session_id="ops1", platform="cli")
    clock.t += 1
    mine.pre_llm_call(session_id="busy", platform="gadget")
    ops.pre_approval_request(session_id="ops2")
    assert [r["label"] for r in mine.sessions(ops)] == ["session ops2", "gadget busy", "cli ops1", "cli mine"]
    assert [r["label"] for r in mine.sessions()] == ["gadget busy", "cli mine"]
    for n in range(dashboard.MAX_SESSIONS):
        ops.on_session_start(session_id=f"x{n:03d}")
    assert len(mine.sessions(ops)) == dashboard.MAX_SESSIONS


def test_listeners_hear_changes_only():
    board = SessionBoard()
    calls = []
    unsubscribe = board.subscribe(lambda: calls.append(1))
    board.pre_llm_call(session_id="s1", platform="cli")
    board.pre_llm_call(session_id="s1", platform="cli")  # still busy: no change
    board.on_session_end(session_id="s1")
    assert len(calls) == 2
    unsubscribe()
    board.pre_llm_call(session_id="s1")
    assert len(calls) == 2


def test_a_failing_listener_never_reaches_the_agent():
    board = SessionBoard()

    def broken():
        raise RuntimeError("boom")

    board.subscribe(broken)
    assert board.pre_llm_call(session_id="s1") is None  # pre_llm_call results become turn context


def test_the_board_registers_every_hook_it_handles(monkeypatch):
    from hermes_gadget_plugin import runtime

    monkeypatch.setitem(runtime._boards, "default", None)  # its registration is undone after the test

    class Ctx:
        def __init__(self):
            self.hooks = {}

        def register_hook(self, name, callback):
            self.hooks[name] = callback

    ctx = Ctx()
    SessionBoard().register(ctx)
    assert set(ctx.hooks) == {"on_session_start", "pre_llm_call", "on_session_end", "pre_approval_request",
                              "post_approval_response", "on_session_finalize", "on_session_reset"}
    # Hermes adds keyword arguments over time; the hooks take whatever comes.
    ctx.hooks["pre_llm_call"](session_id="x", future_field=1, telemetry_schema_version=2)


def test_each_profiles_copy_registers_its_board_by_profile(monkeypatch):
    from types import SimpleNamespace

    from hermes_gadget_plugin import runtime

    monkeypatch.setitem(runtime._boards, "ops", None)
    board = SessionBoard()
    board.register(SimpleNamespace(profile_name="ops", register_hook=lambda name, callback: None))
    assert runtime.boards(["ops"]) == [board]  # what the default profile's adapter reads for ops


# -- tiles -------------------------------------------------------------------------------------

def test_tiles_are_set_listed_and_cleared_across_processes(tmp_path):
    cli, gateway = TileStore(tmp_path), TileStore(tmp_path)  # two processes, one file
    cli.set("nas", "up", state="ok")
    cli.set("backups", "2 days", state="warn", label="Backups")
    assert gateway.tiles() == [{"id": "nas", "label": "nas", "value": "up", "state": "ok"},
                               {"id": "backups", "label": "Backups", "value": "2 days", "state": "warn"}]
    cli.set("nas", "down", state="bad")  # keeps its place
    assert [t["value"] for t in gateway.tiles()] == ["down", "2 days"]
    assert cli.clear("nas") and not cli.clear("nas")
    assert cli.clear(None) and gateway.tiles() == []
    assert not cli.clear(None)


def test_tiles_expire_and_are_validated(tmp_path):
    clock = Clock()
    tiles = TileStore(tmp_path, clock=clock)
    tiles.set("ping", "12 ms", ttl_s=60)
    assert len(tiles.tiles()) == 1
    clock.t += 61
    assert tiles.tiles() == []
    with pytest.raises(ValueError):
        tiles.set("x", "y", state="purple")
    with pytest.raises(ValueError):
        tiles.set("  ", "y")
    for i in range(dashboard.MAX_TILES):
        tiles.set(f"t{i}", "v")
    with pytest.raises(ValueError):
        tiles.set("one-too-many", "v")
    tiles.set("t0", "changed")  # an existing tile can still change


def test_an_unreadable_tile_file_shows_no_tiles(tmp_path):
    (tmp_path / TileStore.FILENAME).write_text("{not json", encoding="utf-8")
    assert TileStore(tmp_path).tiles() == []


def test_the_message_is_folded_and_cut_for_the_device():
    msg = dashboard.message([{"id": "abcd", "label": "telegram — \U0001f600 abcd", "state": "busy"}],
                            [{"id": "nas", "label": "NAS\nbox", "value": "x" * 80, "state": "ok"}],
                            charset="ascii", cols=20)
    assert msg["type"] == "dashboard" and msg["v"] == dashboard.VERSION
    assert msg["sessions"] == [{"id": "abcd", "label": "telegram - abcd", "state": "busy"}]
    assert msg["tiles"][0]["label"] == "NAS box"
    assert msg["tiles"][0]["value"] == "x" * 20
    assert json.dumps(msg).isascii()


# -- delivery through the hub -----------------------------------------------------------------

SNAPSHOT = dashboard.message([{"id": "abcd", "label": "cli abcd", "state": "busy"}], [])
NEWER = dashboard.message([{"id": "abcd", "label": "cli abcd", "state": "idle"}], [])


@pytest.fixture
def online(devserver, loop_thread):
    hub, _brain, url = devserver()
    devices = []

    def connect(**kw):
        device = Device(url, **{"dashboard": dashboard.VERSION, **kw})  # replays need a device that draws them
        devices.append(device)
        assert _wait(lambda: device.device_id in hub.sessions)
        return device, hub.sessions[device.device_id]

    yield hub, connect
    for d in devices:
        d.close()


def dashboards(frames: list) -> list[dict]:
    return [f for f in frames if isinstance(f, dict) and f.get("type") == "dashboard"]


def test_an_unchanged_dashboard_is_not_sent_again(online, loop_thread):
    _hub, connect = online
    device, session = connect()
    device.frames()
    assert loop_thread.run(session.send_dashboard(SNAPSHOT)) is True
    assert loop_thread.run(session.send_dashboard(dict(SNAPSHOT))) is False
    assert loop_thread.run(session.send_dashboard(NEWER)) is True
    assert dashboards(device.frames()) == [SNAPSHOT, NEWER]


def test_a_dark_screen_gets_only_the_latest_dashboard_when_it_lights_up(online, loop_thread):
    hub, connect = online
    device, session = connect()
    device.frames()
    device.screen(True)
    assert _wait(lambda: session.display_sleeping)
    assert loop_thread.run(session.send_dashboard(SNAPSHOT)) is False
    assert loop_thread.run(session.send_dashboard(NEWER)) is False
    loop_thread.run(session.show_card("Timer", "9 min", ttl_s=0))
    assert dashboards(device.frames()) == []
    device.screen(False)
    frames = device.frames()
    assert dashboards(frames) == [NEWER]
    assert types(frames) == ["dashboard", "display"]  # the card stays on top of it


def test_a_dashboard_sent_while_the_held_status_goes_out_replaces_the_held_one(online, loop_thread):
    import asyncio

    hub, connect = online
    device, session = connect()
    device.frames()
    device.screen(True)
    assert _wait(lambda: session.display_sleeping)
    loop_thread.run(session.send_status("Thinking"))
    loop_thread.run(session.send_dashboard(SNAPSHOT))
    loop_thread.run(session._send_lock.acquire())  # another write is still going out
    device.screen(False)  # the wake queues the held status behind it
    assert _wait(lambda: not session.display_sleeping)
    time.sleep(0.1)
    newer = asyncio.run_coroutine_threadsafe(session.send_dashboard(NEWER), loop_thread.loop)
    time.sleep(0.1)
    loop_thread.loop.call_soon_threadsafe(session._send_lock.release)
    newer.result(5)
    frames = device.frames()
    assert types(frames)[0] == "status" and dashboards(frames) == [NEWER]  # never the older one after it



def test_a_dashboard_held_for_a_device_that_is_then_unpaired_is_never_sent(online, loop_thread):
    hub, connect = online
    device, session = connect()
    loop_thread.run(session.set_paired(True))
    device.frames()
    device.screen(True)
    assert _wait(lambda: session.display_sleeping)
    loop_thread.run(session.send_dashboard(SNAPSHOT))
    loop_thread.run(session.set_paired(False))  # revoked while the screen was dark
    device.screen(False)
    assert dashboards(device.frames()) == []


def test_a_held_dashboard_survives_a_reconnect(online, loop_thread):
    hub, connect = online
    device, session = connect(sleeping=True)
    loop_thread.run(session.send_dashboard(SNAPSHOT))
    device.close()
    assert _wait(lambda: device.device_id not in hub.sessions)
    again, session2 = connect(key=device.key, sleeping=False)
    assert dashboards(again.frames()) == [SNAPSHOT]
    # A new connection starts with nothing delivered, so the same snapshot goes out again.
    assert loop_thread.run(session2.send_dashboard(NEWER)) is True


def test_a_held_dashboard_is_not_replayed_to_a_device_that_reconnects_unpaired(devserver, loop_thread):
    hub, brain, url = devserver()
    device = Device(url, sleeping=True, dashboard=True)
    assert _wait(lambda: device.device_id in hub.sessions)
    loop_thread.run(hub.sessions[device.device_id].send_dashboard(SNAPSHOT))
    device.close()
    assert _wait(lambda: device.device_id not in hub.sessions)
    brain.require_pairing = True  # revoked while it was away
    again = Device(url, key=device.key, sleeping=False, dashboard=True)
    try:
        assert _wait(lambda: device.device_id in hub.sessions)
        assert dashboards(again.frames()) == []
    finally:
        again.close()


def test_a_held_dashboard_is_not_replayed_to_a_connection_that_cannot_draw_one(devserver, loop_thread):
    hub, _brain, url = devserver()
    device = Device(url, sleeping=True, dashboard=True)
    assert _wait(lambda: device.device_id in hub.sessions)
    loop_thread.run(hub.sessions[device.device_id].send_dashboard(SNAPSHOT))
    device.close()
    assert _wait(lambda: device.device_id not in hub.sessions)
    again = Device(url, key=device.key, sleeping=False)  # paired, lit, and no `dashboard` in its caps
    try:
        assert _wait(lambda: device.device_id in hub.sessions)
        assert hub.sessions[device.device_id].paired
        assert dashboards(again.frames()) == []
    finally:
        again.close()


def test_capabilities_name_the_dashboard_version(online):
    _hub, connect = online
    _device, session = connect(dashboard=False)
    assert session.dashboard_version == 0  # this test device doesn't draw one
    session.caps["dashboard"] = {"v": 1, "rows": 6}
    assert session.dashboard_version == 1
    session.caps["dashboard"] = {"v": True}
    assert session.dashboard_version == 0


# -- widgets -----------------------------------------------------------------------------------

KITCHEN, DESK = "hg-00000000000000a1", "hg-00000000000000b2"


def test_widgets_are_set_listed_and_cleared_across_processes_next_to_the_tiles(tmp_path):
    cli, gateway = TileStore(tmp_path), TileStore(tmp_path)  # two processes, one file
    cli.set("nas", "up", state="ok")
    cli.set_widget("office", "value", value=21.5, unit="C", label="Office", state="ok")
    cli.set_widget("backup", "progress", percent="62.4")
    cli.set_widget("today", "list", items=["09:00 Standup", " 11:30  Dentist "])
    cli.set_widget("playing", "headline", title="Blue in Green", text="Miles Davis")
    assert gateway.widgets() == [
        {"id": "office", "kind": "value", "label": "Office", "value": "21.5", "unit": "C", "state": "ok"},
        {"id": "backup", "kind": "progress", "label": "backup", "percent": 62},
        {"id": "today", "kind": "list", "label": "today", "items": ["09:00 Standup", "11:30 Dentist"]},
        {"id": "playing", "kind": "headline", "label": "playing", "title": "Blue in Green", "text": "Miles Davis"},
    ]
    # Tiles and widgets share the file; writing one keeps the other.
    cli.set("nas", "down", state="bad")
    assert len(gateway.widgets()) == 4 and gateway.tiles()[0]["value"] == "down"
    cli.set_widget("office", "value", value="22")  # the same name: replaced in place, its old fields gone
    assert gateway.widgets()[0] == {"id": "office", "kind": "value", "label": "office", "value": "22"}
    assert gateway.tiles()[0]["value"] == "down"
    assert cli.clear_widgets("backup") and not cli.clear_widgets("backup")
    assert [w["id"] for w in gateway.widgets()] == ["office", "today", "playing"]
    assert cli.clear_widgets() and gateway.widgets() == [] and not cli.clear_widgets()
    assert gateway.tiles() == [{"id": "nas", "label": "nas", "value": "down", "state": "bad"}]
    assert cli.clear(None) and json.loads(cli.path.read_text(encoding="utf-8")) == {"tiles": {}}


def test_a_device_gets_the_shared_widgets_with_its_own_over_them(tmp_path):
    store = TileStore(tmp_path)
    store.set_widget("temp", "value", value="21", label="House")
    store.set_widget("backup", "progress", percent=10)
    store.set_widget("temp", "value", value="24", label="Kitchen", device=KITCHEN)
    store.set_widget("oven", "headline", title="Oven on", device=KITCHEN)
    assert [(w["id"], w["label"]) for w in store.widgets()] == [("temp", "House"), ("backup", "backup")]
    assert store.widgets(DESK) == store.widgets()
    # Its own widget takes the shared one's place; the rest of its own follow.
    assert [(w["id"], w["label"]) for w in store.widgets(KITCHEN)] == [
        ("temp", "Kitchen"), ("backup", "backup"), ("oven", "oven")]
    assert set(store.widget_scopes()) == {"*", KITCHEN}
    # Clearing is per scope: the device's own goes, the shared one shows again.
    assert not store.clear_widgets("oven")
    assert store.clear_widgets("temp", device=KITCHEN)
    assert [(w["id"], w["label"]) for w in store.widgets(KITCHEN)] == [
        ("temp", "House"), ("backup", "backup"), ("oven", "oven")]
    assert store.clear_widgets(device=KITCHEN) and store.widgets(KITCHEN) == store.widgets()
    assert set(store.widget_scopes()) == {"*"}


def test_widgets_expire_and_are_validated_and_limited(tmp_path):
    clock = Clock()
    store = TileStore(tmp_path, clock=clock)
    store.set_widget("ping", "value", value="12 ms", ttl_s=60)
    assert len(store.widgets()) == 1
    clock.t += 61
    assert store.widgets() == [] and store.widget_scopes() == {}
    for bad in (dict(name=" ", kind="value", value="1"), dict(name="x", kind="gauge", value="1"),
                dict(name="x", kind="value"), dict(name="x", kind="value", value="  "),
                dict(name="x", kind="value", value="1", state="purple"),
                dict(name="x", kind="progress"), dict(name="x", kind="progress", percent="half"),
                dict(name="x", kind="progress", percent=float("nan")), dict(name="x", kind="progress", percent=True),
                dict(name="x", kind="list"), dict(name="x", kind="list", items=[" ", ""]),
                dict(name="x", kind="list", items="one string"), dict(name="x", kind="list", items=list("abcde")),
                dict(name="x", kind="headline"), dict(name="x", kind="headline", text="only a second line")):
        with pytest.raises(ValueError):
            store.set_widget(**bad)
    assert store.widgets() == []  # nothing a device can't draw was stored
    # A percent outside 0 to 100 is pulled in; long text is cut.
    assert store.set_widget("over", "progress", percent=250)["percent"] == 100
    assert store.set_widget("under", "progress", percent=-3)["percent"] == 0
    assert len(store.set_widget("long", "headline", title="x" * 999)["title"]) == dashboard.MAX_TEXT
    assert store.clear_widgets()

    # Eight for one device, the shared ones counted with its own.
    for i in range(6):
        store.set_widget(f"s{i}", "value", value=i)
    store.set_widget("k0", "value", value=0, device=KITCHEN)
    store.set_widget("k1", "value", value=1, device=KITCHEN)
    with pytest.raises(ValueError, match="8 widgets"):
        store.set_widget("k2", "value", value=2, device=KITCHEN)
    store.set_widget("k1", "value", value="changed", device=KITCHEN)  # an existing one can still change
    store.set_widget("s0", "value", value="mine", device=KITCHEN)  # so can a device's own over a shared one
    assert store.clear_widgets(device=KITCHEN)
    store.set_widget("s6", "value", value=6)
    store.set_widget("s7", "value", value=7)  # eight shared: the limit for a device with none of its own
    with pytest.raises(ValueError, match="8 widgets"):
        store.set_widget("s8", "value", value=8)
    assert len(store.widgets()) == 8 and len(store.widgets(KITCHEN)) == 8
    # An expired widget frees its place.
    store.clear_widgets()
    store.set_widget("brief", "value", value=1, ttl_s=10)
    for i in range(7):
        store.set_widget(f"t{i}", "value", value=i)
    clock.t += 11
    store.set_widget("after", "value", value=1)
    assert len(store.widgets()) == 8


def test_a_hand_edited_widget_file_never_reaches_a_device_malformed(tmp_path):
    path = tmp_path / TileStore.FILENAME
    path.write_text(json.dumps({"tiles": [], "widgets": {
        "*": {"a": {"kind": "progress", "percent": "lots"}, "b": {"kind": "list", "items": "abc"},
              "c": {"kind": "gauge"}, "d": "nope", "e": {"kind": "value", "value": 7, "state": "purple"},
              "f": {"kind": "progress", "percent": 1e400}},
        KITCHEN: ["not", "a", "table"]}}), encoding="utf-8")
    store = TileStore(tmp_path)
    assert store.tiles() == []
    assert store.widgets(KITCHEN) == [
        {"id": "a", "kind": "progress", "label": "a", "percent": 0},
        {"id": "b", "kind": "list", "label": "b", "items": []},
        {"id": "e", "kind": "value", "label": "e", "value": "7"},
        {"id": "f", "kind": "progress", "label": "f", "percent": 0},
    ]
    json.dumps(dashboard.message([], [], store.widgets()), allow_nan=False)
    path.write_text("{not json", encoding="utf-8")
    assert store.widgets() == [] and store.widget_scopes() == {}


WIDGETS = [
    {"id": "office", "kind": "value", "label": "Büro\nOst", "value": "21,5 — ok", "unit": "°C", "state": "ok"},
    {"id": "backup", "kind": "progress", "label": "Backup", "percent": 62, "state": "warn"},
    {"id": "today", "kind": "list", "label": "Today", "items": ["x" * 80, "11:30 Zahnärztin", "c", "d", "e"]},
    {"id": "playing", "kind": "headline", "label": "Now playing", "title": "T" * 80, "text": "Miles\nDavis"},
]


def test_layout_2_carries_widgets_folded_and_cut_for_the_device():
    msg = dashboard.message([], [], WIDGETS, charset="ascii", cols=20)
    assert msg["v"] == 2 and msg["tiles"] == []
    value, progress, listed, headline = msg["widgets"]
    assert value == {"id": "office", "kind": "value", "label": "Buro Ost", "value": "21,5 - ok", "unit": "degC",
                     "state": "ok"}
    assert progress == WIDGETS[1]  # numbers pass through
    assert listed["items"] == ["x" * 20, "11:30 Zahnarztin", "c", "d"]  # four lines, each cut
    assert headline["title"] == "T" * 20 and headline["text"] == "Miles Davis"
    assert json.dumps(msg).isascii()
    latin = dashboard.message([], [], WIDGETS, charset="latin1", cols=20)["widgets"][0]
    assert latin["label"] == "Büro Ost" and latin["unit"] == "°C"
    # No more than a device takes, and a message without widgets says so: it is a whole snapshot.
    many = [{"id": str(i), "kind": "value", "label": "n", "value": str(i)} for i in range(12)]
    assert len(dashboard.message([], [], many)["widgets"]) == dashboard.MAX_WIDGETS
    assert dashboard.message([], [])["widgets"] == []
    assert dashboard.message([], [], version=7)["v"] == 2  # never a layout this plugin doesn't know


def test_a_layout_1_device_gets_widgets_as_tiles_on_its_list():
    tiles = [{"id": "nas", "label": "NAS", "value": "up", "state": "ok"}]
    msg = dashboard.message([], tiles, WIDGETS, charset="ascii", cols=20, version=1)
    assert msg["v"] == 1 and "widgets" not in msg
    assert msg["tiles"] == [
        {"id": "nas", "label": "NAS", "value": "up", "state": "ok"},
        {"id": "office", "label": "Buro Ost", "value": "21,5 - ok degC", "state": "ok"},
        {"id": "backup", "label": "Backup", "value": "62%", "state": "warn"},
        {"id": "today", "label": "Today", "value": "x" * 20},
        {"id": "playing", "label": "Now playing", "value": "T" * 20},
    ]
    # Tiles come first and keep their places; widgets take what is left of the eight.
    full = [{"id": f"t{i}", "label": "t", "value": "v"} for i in range(7)]
    assert [t["id"] for t in dashboard.message([], full, WIDGETS, version=1)["tiles"]][-2:] == ["t6", "office"]
    full.append({"id": "t7", "label": "t", "value": "v"})
    assert len(dashboard.message([], full, WIDGETS, version=1)["tiles"]) == 8


def test_a_held_layout_2_dashboard_is_not_replayed_to_a_connection_that_draws_layout_1(devserver, loop_thread):
    hub, _brain, url = devserver()
    device = Device(url, sleeping=True, dashboard=2)
    assert _wait(lambda: device.device_id in hub.sessions)
    assert hub.sessions[device.device_id].dashboard_version == 2
    held = dashboard.message([], [], WIDGETS[:1])
    loop_thread.run(hub.sessions[device.device_id].send_dashboard(held))
    device.close()
    assert _wait(lambda: device.device_id not in hub.sessions)
    older = Device(url, key=device.key, sleeping=False, dashboard=1)  # the same device on older firmware
    try:
        assert _wait(lambda: device.device_id in hub.sessions)
        assert dashboards(older.frames()) == []
    finally:
        older.close()


# -- hermes gadget widget ----------------------------------------------------------------------

@pytest.fixture
def widget_cli(tmp_path, monkeypatch):
    import argparse

    from hermes_gadget_plugin import cli
    from hermes_gadget_plugin.store import DeviceStore

    DeviceStore(tmp_path).enroll(KITCHEN, b"k" * 32, name="Kitchen", board="test", address="127.0.0.1")
    monkeypatch.setattr(cli, "_tiles", lambda: TileStore(tmp_path))
    monkeypatch.setattr(cli, "_store", lambda: DeviceStore(tmp_path))
    parser = argparse.ArgumentParser(prog="hermes gadget")
    cli.setup_argparse(parser)

    def run(*words: str) -> None:
        args = parser.parse_args(list(words))
        args.func(args)

    return run, TileStore(tmp_path)


def test_the_widget_command_sets_lists_and_clears_for_every_gadget_or_one(widget_cli, capsys):
    run, store = widget_cli
    run("widget", "list")
    assert "No widgets" in capsys.readouterr().out
    run("widget", "set", "office", "--value", "21.5", "--unit", "C", "--label", "Office", "--state", "ok")
    run("widget", "set", "backup", "--percent", "62", "--ttl", "300")
    run("widget", "set", "today", "--item", "09:00 Standup", "--item", "11:30 Dentist")
    run("widget", "set", "playing", "--title", "Blue in Green", "--text", "Miles Davis", "--device", "kitchen")
    assert "for Kitchen" in capsys.readouterr().out
    assert [w["kind"] for w in store.widgets()] == ["value", "progress", "list"]
    assert [w["kind"] for w in store.widgets(KITCHEN)] == ["value", "progress", "list", "headline"]
    assert store.widgets(KITCHEN)[3] == {"id": "playing", "kind": "headline", "label": "playing",
                                         "title": "Blue in Green", "text": "Miles Davis"}
    run("widget", "list")
    listing = capsys.readouterr().out.splitlines()
    assert len(listing) == 4
    assert listing[0].split() == ["office", "value", "all", "Office", "21.5", "C", "ok"]
    assert listing[3].split()[:3] == ["playing", "headline", "Kitchen"]

    run("widget", "clear", "playing")  # set for the kitchen, not for every gadget
    assert "No such widget" in capsys.readouterr().out
    run("widget", "clear", "playing", "--device", KITCHEN)
    assert store.widgets(KITCHEN) == store.widgets()
    run("widget", "clear", "--all")
    assert store.widgets() == []


def test_the_widget_command_refuses_what_it_cannot_draw(widget_cli, capsys):
    run, store = widget_cli
    for words in (("widget", "set", "x"),  # no kind
                  ("widget", "set", "x", "--value", "1", "--percent", "2"),  # two kinds
                  ("widget", "set", "x", "--percent", "nan"),
                  ("widget", "set", "x", "--item", "a", "--item", "b", "--item", "c", "--item", "d", "--item", "e"),
                  ("widget", "set", "x", "--value", "1", "--device", "attic"),  # no such gadget
                  ("widget", "clear")):
        with pytest.raises(SystemExit) as exit_:
            run(*words)
        assert exit_.value.code not in (0, None)
    assert store.widgets() == []


def test_the_example_feeder_runs_commands_the_cli_takes(widget_cli, capsys):
    import runpy
    from pathlib import Path

    run, store = widget_cli
    example = runpy.run_path(str(Path(__file__).parent.parent / "examples" / "widgets" / "system_widgets.py"))
    commands = example["commands"](str(Path(__file__).anchor or "/"), ttl=60, device="Kitchen")
    assert len(commands) >= 2
    for cmd in commands:
        assert cmd[:2] == ["hermes", "gadget"]
        run(*cmd[2:])
    kinds = {w["id"]: w["kind"] for w in store.widgets(KITCHEN)}
    assert kinds["disk"] == "progress" and kinds["host"] == "headline"
    assert store.widgets() == []  # --device kept them to the kitchen
    assert example["main"](["--dry-run"]) == 0
    assert capsys.readouterr().out.count("hermes gadget widget set") == len(commands)


# -- the gadget_widget tool --------------------------------------------------------------------

class _FakeAdapter:
    """What the tool reads of the adapter: its tile store, the enrolled devices and the wake."""

    def __init__(self, tmp_path, sessions=()):
        from types import SimpleNamespace

        from hermes_gadget_plugin.store import DeviceStore

        store = DeviceStore(tmp_path)
        store.enroll(KITCHEN, b"k" * 32, name="Kitchen", board="test", address="127.0.0.1")
        store.enroll(DESK, b"d" * 32, name="Desk", board="test", address="127.0.0.1")
        self._tiles = TileStore(tmp_path)
        self.hub = SimpleNamespace(store=store, sessions={s.device_id: s for s in sessions})
        self.woken = 0

    def _board_changed(self) -> None:
        self.woken += 1


@pytest.fixture
def widget_tool(tmp_path, monkeypatch):
    from types import SimpleNamespace

    from hermes_gadget_plugin import tools

    adapter = _FakeAdapter(tmp_path, [
        SimpleNamespace(device_id=KITCHEN, name="Kitchen", paired=True, dashboard_version=2),
        SimpleNamespace(device_id=DESK, name="Desk", paired=True, dashboard_version=0)])
    monkeypatch.setattr(tools, "_adapter", lambda: adapter)
    return (lambda args: json.loads(tools.gadget_widget(args))), adapter


def test_the_widget_tool_sets_and_clears_widgets_and_wakes_the_dashboard(widget_tool):
    call, adapter = widget_tool
    done = call({"name": "weather", "kind": "headline", "title": "Rain by 5", "text": "12 C", "ttl_s": 3600})
    assert done == {"success": True, "device": "every gadget", "widget": "weather", "shows": "Rain by 5",
                    "connected_gadgets_showing_it": ["Kitchen"]}  # the Desk draws no dashboard
    assert adapter.woken == 1
    call({"action": "set", "name": "oven", "kind": "progress", "percent": 40, "state": "warn", "device": "kitchen"})
    call({"name": "cal", "kind": "list", "items": ["09:00 Standup"], "device": DESK})
    assert [w["id"] for w in adapter._tiles.widgets(KITCHEN)] == ["weather", "oven"]
    assert [w["id"] for w in adapter._tiles.widgets(DESK)] == ["weather", "cal"]
    listed = call({"action": "list", "device": "Kitchen"})
    assert listed["device"] == "Kitchen" and [w["id"] for w in listed["widgets"]] == ["weather", "oven"]

    assert call({"action": "clear", "name": "oven"})["cleared"] is False  # set for the kitchen only
    assert call({"action": "clear", "name": "oven", "device": "Kitchen"})["cleared"] is True
    assert call({"action": "clear"})["cleared"] is True  # every widget set for every gadget
    assert adapter._tiles.widgets(KITCHEN) == [] and [w["id"] for w in adapter._tiles.widgets(DESK)] == ["cal"]


def test_the_widget_tool_explains_what_it_refuses(widget_tool, monkeypatch):
    from hermes_gadget_plugin import tools

    call, adapter = widget_tool
    woken = adapter.woken
    for args, said in (({"name": "x", "kind": "value"}, "needs a value"),
                       ({"name": "x"}, "kind must be one of"),
                       ({"kind": "value", "value": "1"}, "needs a name"),
                       ({"name": "x", "kind": "list", "items": list("abcde")}, "holds 4 items"),
                       ({"name": "x", "kind": "value", "value": "1", "ttl_s": -5}, "ttl must be"),
                       ({"name": "x", "kind": "value", "value": "1", "ttl_s": "soon"}, "ttl must be"),
                       ({"name": "x", "kind": "value", "value": "1", "device": "attic"}, "no single gadget"),
                       ({"action": "shuffle"}, "action must be")):
        answer = call(args)
        assert answer["success"] is False and said in answer["error"], args
    assert adapter._tiles.widgets() == [] and adapter.woken == woken
    adapter._tiles = None  # platforms.gadget.extra.dashboard: false
    assert "turned off" in call({"name": "x", "kind": "value", "value": "1"})["error"]
    monkeypatch.setattr(tools, "_adapter", lambda: None)
    assert "not running" in call({"name": "x", "kind": "value", "value": "1"})["error"]


# -- review fixes: concurrent writers, the limit across scopes, ttl, forget --------------------

def test_threads_setting_widgets_and_tiles_at_once_lose_nothing(tmp_path):
    import threading

    TileStore(tmp_path).set("nas", "up", state="ok")
    TileStore(tmp_path).set_widget("kept", "value", value="before")
    errors: list[BaseException] = []

    def writer(i: int) -> None:
        store = TileStore(tmp_path)  # as the tool, the gateway and the CLI each have their own
        try:
            for n in range(25):
                if i % 2:
                    store.set_widget(f"w{i}", "value", value=n)
                else:
                    store.set(f"t{i}", str(n))
                store.widgets()
                store.tiles()
        except BaseException as exc:  # noqa: BLE001 - the test reports whatever a writer met
            errors.append(exc)

    threads = [threading.Thread(target=writer, args=(i,)) for i in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    store = TileStore(tmp_path)
    assert {w["id"]: w["value"] for w in store.widgets()} == {"kept": "before", "w1": "24", "w3": "24", "w5": "24"}
    assert {t["id"]: t["value"] for t in store.tiles()} == {"nas": "up", "t0": "24", "t2": "24", "t4": "24"}
    assert sorted(p.name for p in tmp_path.iterdir()) == [TileStore.FILENAME]  # no temporary or lock file left


def test_processes_setting_widgets_and_tiles_at_once_lose_nothing(tmp_path):
    import subprocess
    import sys
    from pathlib import Path

    TileStore(tmp_path).set("nas", "up", state="ok")
    TileStore(tmp_path).set_widget("kept", "value", value="before")
    plugin_parent = Path(dashboard.__file__).resolve().parent.parent
    script = (
        "import sys\n"
        f"sys.path.insert(0, {str(plugin_parent)!r})\n"
        f"from {dashboard.__name__} import TileStore\n"
        "store, who = TileStore(sys.argv[1]), sys.argv[2]\n"
        "for n in range(40):\n"
        "    if who == 'tiles':\n"
        "        store.set('t', str(n))\n"
        "    else:\n"
        "        store.set_widget(who, 'value', value=n)\n"
    )
    procs = [subprocess.Popen([sys.executable, "-c", script, str(tmp_path), who], stderr=subprocess.PIPE)
             for who in ("a", "b", "tiles")]
    failures = [p.communicate(timeout=120)[1].decode(errors="replace") for p in procs]
    assert [p.returncode for p in procs] == [0, 0, 0], failures
    store = TileStore(tmp_path)
    assert {w["id"]: w["value"] for w in store.widgets()} == {"kept": "before", "a": "39", "b": "39"}
    assert {t["id"]: t["value"] for t in store.tiles()} == {"nas": "up", "t": "39"}


def test_a_file_that_cannot_be_read_is_not_taken_for_an_empty_one(tmp_path, monkeypatch):
    from pathlib import Path

    store = TileStore(tmp_path)
    store.set("nas", "up")
    store.set_widget("kept", "value", value="before")
    before = store.path.read_text(encoding="utf-8")
    real_read, calls = Path.read_text, []

    def busy(self, *args, **kw):  # another process holds the file: Windows says PermissionError
        if self == store.path:
            calls.append(1)
            if len(calls) <= busy.times:
                raise PermissionError(13, "busy")
        return real_read(self, *args, **kw)

    monkeypatch.setattr(Path, "read_text", busy)
    monkeypatch.setattr(dashboard.time, "sleep", lambda s: None)
    busy.times = 3  # it frees up within a few tries: the write goes through, with everything kept
    store.set_widget("new", "value", value="1")
    assert [w["id"] for w in store.widgets()] == ["kept", "new"] and store.tiles()[0]["id"] == "nas"
    busy.times = 10 ** 6  # it never does: an error, and the file as it was
    calls.clear()
    after_new = real_read(store.path, encoding="utf-8")
    for change in (lambda: store.set_widget("lost", "value", value="1"), lambda: store.set("lost", "1"),
                   lambda: store.clear_widgets(), lambda: store.clear(None)):
        with pytest.raises(OSError):
            change()
    with pytest.raises(OSError):
        store.widgets()  # the gateway keeps what devices show rather than sending them nothing
    assert real_read(store.path, encoding="utf-8") == after_new != before


def test_the_widget_tool_and_command_report_a_file_they_cannot_write(widget_tool, widget_cli, monkeypatch):
    call, adapter = widget_tool

    def refuse(*args, **kw):
        raise PermissionError(13, "busy")

    monkeypatch.setattr(TileStore, "_save", refuse)
    answer = call({"name": "x", "kind": "value", "value": "1"})
    assert answer["success"] is False and "busy" in answer["error"]
    assert call({"action": "clear"})["success"] is True  # nothing to clear writes nothing
    run, _store = widget_cli
    with pytest.raises(SystemExit) as exit_:
        run("widget", "set", "x", "--value", "1")
    assert "busy" in str(exit_.value.code)


def test_a_shared_widget_never_pushes_a_gadgets_own_off_its_screen(tmp_path):
    store = TileStore(tmp_path)
    for i in range(6):
        store.set_widget(f"s{i}", "value", value=i)
    store.set_widget("k0", "value", value="mine", device=KITCHEN)
    store.set_widget("k1", "value", value="mine", device=KITCHEN)
    with pytest.raises(ValueError, match=KITCHEN):  # the kitchen has no room: it says which gadget
        store.set_widget("s6", "value", value=6)
    assert [w["id"] for w in store.widgets(KITCHEN)] == ["s0", "s1", "s2", "s3", "s4", "s5", "k0", "k1"]
    assert len(store.widgets()) == 6  # refused whole: no other gadget got it either
    store.set_widget("s0", "value", value="changed")  # one every gadget already shows can change
    store.set_widget("k0", "value", value="shared now")  # so can a name the kitchen has of its own
    assert len(store.widgets(KITCHEN)) == 8 and len(store.widgets(DESK)) == 7
    assert store.clear_widgets("k1", device=KITCHEN)
    store.set_widget("s6", "value", value=6)  # room again
    assert len(store.widgets(KITCHEN)) == 8


def test_a_ttl_is_a_positive_number_of_seconds_for_the_store_the_command_and_the_tool(
        tmp_path, widget_cli, widget_tool):
    store = TileStore(tmp_path / "own")
    for bad in (-5, 0, float("nan"), float("inf"), "soon", True):
        with pytest.raises(ValueError, match="ttl"):
            store.set_widget("x", "value", value="1", ttl_s=bad)
        with pytest.raises(ValueError, match="ttl"):
            store.set("x", "1", ttl_s=bad)
    assert store.widgets() == [] and store.tiles() == []
    store.set_widget("x", "value", value="1", ttl_s="90")  # a number as text, as a tool may send it
    assert "NaN" not in store.path.read_text(encoding="utf-8")

    run, shared = widget_cli
    for bad in ("-5", "nan", "0"):
        with pytest.raises(SystemExit) as exit_:
            run("widget", "set", "x", "--value", "1", "--ttl", bad)
        assert "ttl" in str(exit_.value.code)
    assert shared.widgets() == []
    call, _adapter = widget_tool
    assert "ttl" in call({"name": "x", "kind": "value", "value": "1", "ttl_s": float("inf")})["error"]


def test_forgetting_a_gadget_removes_the_widgets_set_for_it(widget_cli, capsys):
    run, store = widget_cli
    run("widget", "set", "shared", "--value", "1")
    run("widget", "set", "oven", "--percent", "40", "--device", "kitchen")
    run("forget", "kitchen")
    assert "widget" in capsys.readouterr().out
    assert set(store.widget_scopes()) == {"*"}
    assert [w["id"] for w in store.widgets(KITCHEN)] == ["shared"]


def test_each_session_gets_its_own_widgets_in_the_layout_it_draws(tmp_path):
    from types import SimpleNamespace

    store = TileStore(tmp_path)
    store.set("nas", "up")
    store.set_widget("temp", "value", value="21", label="Büro")
    store.set_widget("oven", "progress", percent=40, device=KITCHEN)
    sessions = [{"id": "abcd", "label": "cli abcd", "state": "idle"}]
    kitchen = SimpleNamespace(device_id=KITCHEN, charset="ascii", display={"text_cols": 20}, dashboard_version=2)
    desk = SimpleNamespace(device_id=DESK, charset="latin1", display={}, dashboard_version=1)
    newer = SimpleNamespace(device_id=DESK, charset="ascii", display={"text_cols": 20}, dashboard_version=9)
    msg = dashboard.message_for(kitchen, sessions, store)
    assert msg["v"] == 2 and [w["id"] for w in msg["widgets"]] == ["temp", "oven"]
    assert msg["widgets"][0]["label"] == "Buro" and msg["sessions"] == sessions
    old = dashboard.message_for(desk, sessions, store)
    assert old["v"] == 1 and "widgets" not in old
    assert [(t["id"], t["label"]) for t in old["tiles"]] == [("nas", "nas"), ("temp", "Büro")]  # not the kitchen's
    assert dashboard.message_for(newer, sessions, store)["v"] == dashboard.VERSION


def test_a_lock_that_is_held_is_never_taken_over_however_old_its_file_is(tmp_path, monkeypatch):
    # Age is not ownership: a writer that removed an old lock file could remove the lock another
    # writer had just taken, and both then changed the same file.
    import os

    monkeypatch.setattr(dashboard, "LOCK_WAIT_S", 0.3)
    store = TileStore(tmp_path)
    with store._locked():
        lock = tmp_path / (TileStore.FILENAME + ".lock")
        old = time.time() - 3600
        os.utime(lock, (old, old))
        with pytest.raises(TimeoutError):
            with TileStore(tmp_path)._locked():
                pass
    store.set("nas", "up")  # released: the next change goes through
    assert sorted(p.name for p in tmp_path.iterdir()) == [TileStore.FILENAME]


def test_a_lock_file_left_by_a_process_that_died_blocks_nobody(tmp_path):
    (tmp_path / (TileStore.FILENAME + ".lock")).write_bytes(b"")
    TileStore(tmp_path).set("nas", "up")
    assert [t["id"] for t in TileStore(tmp_path).tiles()] == ["nas"]


def test_setting_an_expired_tile_again_counts_as_a_new_one(tmp_path):
    now = [1000.0]
    store = TileStore(tmp_path, clock=lambda: now[0])
    store.set("old", "x", ttl_s=10)
    now[0] += 60
    for i in range(dashboard.MAX_TILES):
        store.set(f"t{i}", str(i))
    with pytest.raises(ValueError, match="clear one first"):
        store.set("old", "back")  # it would be a ninth, and hide the last of the others
    assert len(store.tiles()) == dashboard.MAX_TILES
    store.set("t0", "again")  # a live one is replaced as before
