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
    assert msg["type"] == "dashboard" and msg["v"] == 1
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
        device = Device(url, **{"dashboard": True, **kw})  # replays need a device that draws dashboards
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
