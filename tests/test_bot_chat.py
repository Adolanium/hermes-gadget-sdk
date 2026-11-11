"""`hermes gadget bot-chat <device> [profile]`: the request queue, the checks before it, and the wait after."""

from __future__ import annotations

import sqlite3
import types

import pytest
from conftest import requires_hermes

from hermes_gadget_plugin import botchat, cli
from hermes_gadget_plugin import status as live_status
from hermes_gadget_plugin.store import DeviceStore

DEVICE_ID = "hg-630dcd2966c43366"
KEY = bytes(range(32))


def _state_db(home, chat_id=None, source="tui", title="Bot Chat"):
    """A profile's session store with one session, as Hermes keeps it (only the columns read here)."""
    home.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(home / "state.db")
    conn.execute("CREATE TABLE IF NOT EXISTS sessions (id TEXT, title TEXT, source TEXT, chat_id TEXT, started_at REAL)")
    conn.execute("DELETE FROM sessions")
    if title:
        conn.execute("INSERT INTO sessions VALUES ('20261111_090111_6705fd', ?, ?, ?, 1)", (title, source, chat_id))
    conn.commit()
    conn.close()


def test_the_queue_keeps_one_request_per_device_and_its_report(tmp_path):
    """A request and a report live side by side; ids keep two runs for one device apart."""
    queue = botchat.BotChatQueue(tmp_path)
    assert queue.pending() == []
    first = queue.request(DEVICE_ID, "ops")
    assert queue.pending() == [DEVICE_ID] and queue.load(DEVICE_ID) | {"requested_at": 0} == {
        "id": first, "profile": "ops", "requested_at": 0}
    queue.report(DEVICE_ID, id=first, state="sent", chat_id=f"{DEVICE_ID}~ops")
    assert queue.pending() == [DEVICE_ID]  # a report is not a request
    second = queue.request(DEVICE_ID, "default")  # another run replaces the request
    assert second != first
    assert not queue.drop(DEVICE_ID, first)  # the gateway that loaded the first one leaves the second alone
    assert queue.load(DEVICE_ID)["id"] == second
    assert queue.drop(DEVICE_ID, second) and queue.pending() == []
    assert queue.status(DEVICE_ID)["id"] == first  # the old report stays until a new one replaces it
    with pytest.raises(ValueError):
        queue.request("../../etc", "ops")


def test_bot_chat_row_reads_the_canonical_session(tmp_path):
    """The row titled Bot Chat, read only; none when the store or the row is missing."""
    assert botchat.bot_chat_row(tmp_path / "missing.db") is None
    _state_db(tmp_path, title="Something else")
    assert botchat.bot_chat_row(tmp_path / "state.db") is None
    _state_db(tmp_path, chat_id=DEVICE_ID, source="gadget")
    assert botchat.bot_chat_row(tmp_path / "state.db") == {
        "id": "20261111_090111_6705fd", "source": "gadget", "chat_id": DEVICE_ID}


@pytest.fixture
def rig(tmp_path, monkeypatch):
    """The CLI against a device store, a config and profile homes under tmp_path."""
    pytest.importorskip("hermes_cli.profiles")
    import hermes_cli.profiles as profiles

    store = DeviceStore(tmp_path / "plugin-data")
    store.enroll(DEVICE_ID, KEY, name="Desk", board="raw")
    monkeypatch.setattr(cli, "_store", lambda: store)
    extra = {"allow_admin_from": [DEVICE_ID], "profiles": ["ops"]}
    monkeypatch.setattr(cli, "_gadget_extra", lambda: extra)
    homes = {"default": tmp_path / "home", "ops": tmp_path / "home" / "profiles" / "ops"}
    monkeypatch.setattr(profiles, "get_profile_dir", lambda name: homes.get(name, tmp_path / "nowhere" / name))
    for home in homes.values():
        _state_db(home)
    gateway = []  # what the fake gateway does on each wait tick
    monkeypatch.setattr(cli.time, "sleep", lambda s: gateway and gateway.pop(0)())
    queue = botchat.BotChatQueue(store.path.parent)

    def on(profile):
        """The gateway's live status: the device connected, on ``profile``."""
        live_status.write(store.path.parent, {DEVICE_ID: {"online": True, "profile": profile}})

    def answer(**report):
        """The gateway serves the pending request: drops it and reports, under its id."""
        request = queue.load(DEVICE_ID)
        queue.drop(DEVICE_ID, request["id"])
        queue.report(DEVICE_ID, id=request["id"], **report)

    return types.SimpleNamespace(store=store, extra=extra, homes=homes, gateway=gateway, queue=queue, on=on,
                                 answer=answer)


def _run(profile="default", timeout=5):
    cli._cmd_bot_chat(types.SimpleNamespace(device="Desk", profile=profile, timeout=timeout))


@requires_hermes
def test_a_profile_without_a_bot_chat_says_how_to_make_one(rig):
    """No Bot Chat yet: the command says how to make one and queues nothing."""
    _state_db(rig.homes["ops"], title=None)
    with pytest.raises(SystemExit) as exit_:
        _run("ops")
    assert 'hermes -p ops chat -c "Bot Chat" --create-if-missing' in str(exit_.value)
    assert rig.queue.pending() == []


@requires_hermes
def test_a_gadget_that_is_not_an_admin_gets_the_config_to_add(rig):
    """/resume --all needs an admin: the command prints the YAML that makes the gadget one."""
    rig.extra["allow_admin_from"] = ["hg-0000000000000000"]
    with pytest.raises(SystemExit) as exit_:
        _run()
    assert f"allow_admin_from: [{DEVICE_ID}]" in str(exit_.value)
    assert "user_allowed_commands: [new, stop]" in str(exit_.value)
    assert rig.queue.pending() == []


@requires_hermes
def test_a_profile_the_gadget_is_not_offered_is_refused(rig):
    """A profile the gadget can't switch to is refused; "all" offers it."""
    rig.extra["profiles"] = ["work"]
    with pytest.raises(SystemExit) as exit_:
        _run("ops")
    assert "platforms.gadget.extra.profiles" in str(exit_.value)
    rig.extra["profiles"] = "all"
    _state_db(rig.homes["ops"], chat_id=DEVICE_ID, source="gadget")
    rig.on("ops")
    _run("ops")  # offered by "all", and already there: nothing to queue
    assert rig.queue.pending() == []


@requires_hermes
def test_already_there_only_while_the_device_is_on_that_profile(rig, capsys):
    """The Bot Chat row says where the session went last; the device may have moved on since, and then
    it is switched back rather than told it is already there."""
    _state_db(rig.homes["ops"], chat_id=DEVICE_ID, source="gadget")
    rig.on("default")
    rig.gateway += [lambda: rig.answer(state="sent", profile="ops", chat_id=DEVICE_ID)]
    _run("ops")
    assert "now talks in ops's Bot Chat" in capsys.readouterr().out  # queued and served, not "already"
    rig.on("ops")
    _run("ops")
    assert "already in ops's Bot Chat" in capsys.readouterr().out


@requires_hermes
@pytest.mark.parametrize("virtual", [False, True], ids=["stamp", "virtual"])
def test_it_waits_until_the_bot_chat_is_on_the_gadgets_chat(rig, capsys, virtual):
    """Sent is not done: the command waits until the Bot Chat row is on the gadget's chat."""
    if virtual:
        rig.extra["profile_mode"] = "virtual"
    chat_id = f"{DEVICE_ID}~ops" if virtual else DEVICE_ID

    def gateway_sends():
        assert rig.queue.load(DEVICE_ID)["profile"] == "ops"
        rig.answer(state="sent", profile="ops", chat_id=chat_id)

    rig.gateway += [gateway_sends, lambda: None, lambda: _state_db(rig.homes["ops"], chat_id=chat_id, source="gadget")]
    _run("ops")
    assert "now talks in ops's Bot Chat" in capsys.readouterr().out


@requires_hermes
def test_another_runs_report_is_not_taken_for_this_one(rig, monkeypatch):
    """A report under another request's id (an older run's) is not this run's result."""
    rig.queue.report(DEVICE_ID, id="someone-else", state="sent", profile="ops", chat_id=DEVICE_ID)
    _state_db(rig.homes["ops"], chat_id=DEVICE_ID, source="gadget")
    clock = iter(range(0, 1000, 2))
    monkeypatch.setattr(cli.time, "monotonic", lambda: next(clock))
    with pytest.raises(SystemExit) as exit_:
        _run("ops", timeout=6)
    assert "Stopped waiting" in str(exit_.value) and "10 minutes" in str(exit_.value)
    assert rig.queue.pending() == [DEVICE_ID]  # the request is still there for the gateway


@requires_hermes
def test_a_refused_resume_is_reported_rather_than_waited_out(rig, monkeypatch):
    """Sent, but the row never moved: say so, rather than claim success."""
    def gateway_sends():
        rig.answer(state="sent", profile="default", chat_id=DEVICE_ID)

    rig.gateway += [gateway_sends] + [lambda: None] * 3
    clock = iter(range(0, 1000, 2))
    monkeypatch.setattr(cli.time, "monotonic", lambda: next(clock))
    with pytest.raises(SystemExit) as exit_:
        _run(timeout=6)
    assert "didn't move" in str(exit_.value)


@requires_hermes
def test_a_failed_switch_is_reported(rig):
    """The gateway's failure, under this run's id, ends the wait with its reason."""
    rig.gateway.append(lambda: rig.answer(state="failed", error="cannot switch to ops: unknown"))
    with pytest.raises(SystemExit) as exit_:
        _run("ops")
    assert "cannot switch to ops: unknown" in str(exit_.value)
