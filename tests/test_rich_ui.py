"""Structured cards and choices (docs/protocol.md#display, #questions): what the hub sends a
device that announces ``caps.card`` and ``caps.choice``, the plain card one without them gets
instead, the agent tools on top, and the simulated firmware answering for real."""

from __future__ import annotations

import asyncio
import base64
import json
import secrets
import threading
import time

import pytest

from conftest import requires_sim
from hermes_gadget_plugin import hub as gadget_hub
from hermes_gadget_plugin import protocol, textfmt, tools

CARD = {"v": 1, "items": 12, "qr": 84}
CHOICE = {"v": 1, "options": 6}


class Device:
    """A bare protocol client with the capabilities a test gives it."""

    def __init__(self, url: str, *, caps: dict | None = None, sleeping: bool | None = None, cols: int = 25,
                 key: bytes | None = None):
        from websockets.sync.client import connect

        self.key = key or secrets.token_bytes(32)
        self.device_id = protocol.device_id_for_key(self.key)
        self.ws = connect(url, subprotocols=[protocol.SUBPROTOCOL], open_timeout=5)
        display = {"width": 320, "height": 240, "charset": "latin1", "text_cols": cols, "text_rows": 8}
        hello = protocol.message("hello", proto=protocol.VERSION, device_id=self.device_id, name="watch",
                                 board="test", caps={"display": display, **(caps or {})})
        if sleeping is not None:
            hello["display_sleeping"] = sleeping
        self.ws.send(json.dumps(hello))
        challenge = json.loads(self.ws.recv(timeout=5))
        if challenge["enrolled"]:
            mac = protocol.auth_mac(self.key, self.device_id, challenge["nonce"])
            self.ws.send(json.dumps(protocol.message("auth", mac=mac)))
        else:
            self.ws.send(json.dumps(protocol.message("auth", key=base64.b64encode(self.key).decode())))
        assert json.loads(self.ws.recv(timeout=5))["type"] == "welcome"

    def send(self, type_: str, **fields) -> None:
        self.ws.send(json.dumps(protocol.message(type_, **fields)))

    def frames(self, quiet_s: float = 0.3) -> list[dict]:
        out = []
        while True:
            try:
                frame = json.loads(self.ws.recv(timeout=quiet_s))
            except TimeoutError:
                return out
            if frame.get("type") != "ping":
                out.append(frame)

    def next(self, type_: str, timeout: float = 5.0) -> dict:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                frame = json.loads(self.ws.recv(timeout=0.2))
            except TimeoutError:
                continue
            if frame.get("type") == type_:
                return frame
        raise AssertionError(f"no {type_} within {timeout}s")

    def close(self) -> None:
        self.ws.close()


def _wait(predicate, timeout: float = 5.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.01)
    return False


@pytest.fixture
def online(devserver, loop_thread):
    hub, _brain, url = devserver()
    devices = []

    def connect(**kw) -> tuple[Device, gadget_hub.DeviceSession]:
        device = Device(url, **kw)
        devices.append(device)
        assert _wait(lambda: device.device_id in hub.sessions and hub.sessions[device.device_id].welcomed)
        return device, hub.sessions[device.device_id]

    yield hub, connect
    for d in devices:
        d.close()


# -- cards -----------------------------------------------------------------------------


def test_a_device_with_caps_card_gets_the_structured_card(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"card": CARD})
    assert session.card_items == 12 and session.card_qr_bytes == 84
    assert loop_thread.run(session.show_card(
        "Build", "Compiling", 0, items=["core", "sim"], progress=40, card_id="build")) is True
    assert device.frames() == [{"type": "display", "title": "Build", "body": "Compiling", "ttl_s": 0,
                                "items": ["core", "sim"], "progress": 40, "id": "build"}]
    loop_thread.run(session.show_card("Join", "Scan to open", 0, qr="https://example.org/a"))
    assert device.frames() == [{"type": "display", "title": "Join", "body": "Scan to open", "ttl_s": 0,
                                "qr": "https://example.org/a"}]


def test_a_plain_card_is_what_it_was_and_its_id_goes_only_to_a_device_that_knows_ids(online, loop_thread):
    _hub, connect = online
    plain = {"type": "display", "title": "Timer", "body": "Pasta: 9 min", "ttl_s": 15}
    for caps in ({}, {"card": CARD}):
        device, session = connect(caps=caps)
        loop_thread.run(session.show_card("Timer", "Pasta: 9 min", 15))
        assert device.frames() == [plain]
        loop_thread.run(session.show_card("Timer", "Pasta: 9 min", 15, card_id="t"))
        assert device.frames() == [{**plain, "id": "t"} if caps else plain]


def test_a_device_without_caps_card_gets_the_same_content_as_a_plain_card(online, loop_thread):
    _hub, connect = online
    device, session = connect()
    assert session.card_items == 0 and session.card_qr_bytes == 0
    loop_thread.run(session.show_card("Build", "Compiling", 0, items=["core", "sim"], progress=40,
                                      qr="https://example.org/a", card_id="build"))
    assert device.frames() == [{
        "type": "display", "title": "Build", "ttl_s": 0,
        "body": "[####------] 40%\nCompiling\n* core\n* sim\nhttps://example.org/a"}]
    assert session.card_shown_as(items=["core"]) == "text"


def test_a_code_longer_than_the_device_draws_is_sent_as_text(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"card": {"v": 1, "items": 4, "qr": 20}})
    link = "https://example.org/a-long-link"
    loop_thread.run(session.show_card("Join", "Open", 0, qr=link, items=list("abcdef"), progress=250))
    assert device.frames() == [{"type": "display", "title": "Join", "body": f"Open\n{link}", "ttl_s": 0,
                                "items": ["a", "b", "c", "d"], "progress": 100}]
    assert session.card_shown_as(qr=link) == "qr_as_text"
    assert session.card_shown_as(qr="short") == "card"
    # A device that draws lists but no codes at all: caps.card without qr.
    plain, plain_session = connect(caps={"card": {"v": 1, "items": 12}})
    loop_thread.run(plain_session.show_card("Join", "", 0, qr="abc"))
    assert plain.frames() == [{"type": "display", "title": "Join", "body": "abc", "ttl_s": 0}]


def test_caps_that_are_not_what_the_protocol_says_count_as_none(online):
    _hub, connect = online
    for caps in ({"card": True}, {"card": {"v": 0, "items": 12}}, {"card": {"v": 1, "items": "12"}},
                 {"choice": {"v": 1}}, {"choice": {"options": 6}}, {"choice": 1}):
        _device, session = connect(caps=caps)
        assert session.card_items == 0 and session.choice_options == 0
    _device, session = connect(caps={"card": {"v": 2, "items": 99, "qr": 300}, "choice": {"v": 1, "options": 40}})
    assert session.card_items == gadget_hub.MAX_CARD_ITEMS and session.choice_options == gadget_hub.MAX_CHOICE_OPTIONS


def test_a_structured_card_waits_in_the_dark_and_goes_out_whole(online, loop_thread):
    hub, connect = online
    device, session = connect(caps={"card": CARD}, sleeping=True)
    assert loop_thread.run(session.show_card("Build", "", 0, progress=10, card_id="build")) is False
    assert loop_thread.run(session.show_card("Build", "", 30, progress=60, items=["core"], card_id="build")) is False
    assert device.frames() == []
    hub._held[session.device_id].held_at -= 10  # held for 10 s
    device.send("state", display_sleeping=False)
    shown = device.next("display")
    assert shown["progress"] == 60 and shown["items"] == ["core"] and shown["id"] == "build"
    assert 19 <= shown["ttl_s"] <= 20  # what is left of its time


# -- choices ----------------------------------------------------------------------------


def _ask(loop_thread, session, *args, **kw):
    return asyncio.run_coroutine_threadsafe(session.ask_choice(*args, **kw), loop_thread.loop)


def test_a_choice_goes_out_and_the_picked_index_comes_back(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"choice": CHOICE})
    asked = _ask(loop_thread, session, "Which list?", ["Groceries", "Hardware"], timeout=30)
    choice = device.next("choice")
    assert choice["title"] == "Which list?" and choice["options"] == ["Groceries", "Hardware"]
    assert choice["ttl_s"] == 30 and choice["id"]
    # Answers that are not this choice's are ignored: another id, an index it does not have.
    device.send("choice.reply", id="someone-else", index=0)
    device.send("choice.reply", id=choice["id"], index=2)
    device.send("choice.reply", id=choice["id"], index=True)
    device.send("choice.reply", id=choice["id"], index="1")
    time.sleep(0.2)
    assert not asked.done()
    device.send("choice.reply", id=choice["id"], index=1)
    assert asked.result(5) == 1
    assert device.frames() == []  # answered: nothing to withdraw


def test_only_a_choice_reply_answers_a_choice(online, loop_thread):
    """An action.result or a diag that carries the choice's id is not its answer."""
    _hub, connect = online
    device, session = connect(caps={"choice": CHOICE})
    asked = _ask(loop_thread, session, "Pick", ["a", "b"], timeout=30)
    choice = device.next("choice")
    device.send("action.result", id=choice["id"], ok=True, result={"index": 1})
    device.send("action.result", id=choice["id"], ok=False, error="boom")
    device.send("diag", id=choice["id"], report={}, log="")
    time.sleep(0.3)
    assert not asked.done()
    device.send("choice.reply", id=choice["id"], index=1)
    assert asked.result(5) == 1
    # And a choice.reply does not answer an action that happens to be waiting.
    session.actions = [{"name": "led.set"}]
    acting = asyncio.run_coroutine_threadsafe(session.invoke_action("led.set", {}, timeout=30), loop_thread.loop)
    action = device.next("action")
    device.send("choice.reply", id=action["id"], index=0)
    time.sleep(0.2)
    assert not acting.done()
    device.send("action.result", id=action["id"], ok=True, result={"led": "on"})
    assert acting.result(5) == {"led": "on"}


def test_a_cancelled_choice_returns_none(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"choice": CHOICE})
    asked = _ask(loop_thread, session, "Pick", ["a", "b"])
    device.send("choice.reply", id=device.next("choice")["id"], cancelled=True)
    assert asked.result(5) is None


def test_a_choice_nobody_answers_is_withdrawn(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"choice": CHOICE})
    asked = _ask(loop_thread, session, "Pick", ["a", "b"], timeout=0.3)
    choice = device.next("choice")
    with pytest.raises(gadget_hub.ActionError, match="no answer"):
        asked.result(5)
    assert device.next("choice.close")["id"] == choice["id"]
    # A late answer is to nothing; the next choice is asked afresh.
    device.send("choice.reply", id=choice["id"], index=0)
    again = _ask(loop_thread, session, "Pick", ["a", "b"])
    device.send("choice.reply", id=device.next("choice")["id"], index=0)
    assert again.result(5) == 0


def test_a_choice_needs_the_capability_and_the_right_number_of_options(online, loop_thread):
    _hub, connect = online
    device, session = connect()
    with pytest.raises(gadget_hub.ActionError, match="cannot take a choice"):
        loop_thread.run(session.ask_choice("Pick", ["a", "b"]))
    small, small_session = connect(caps={"choice": {"v": 1, "options": 3}})
    for options in (["only"], ["a", "b", "c", "d"]):
        with pytest.raises(gadget_hub.ActionError, match="2 to 3 options"):
            loop_thread.run(small_session.ask_choice("Pick", options))
    assert device.frames() == [] and small.frames() == []


def test_one_choice_at_a_time_and_a_disconnect_ends_it(online, loop_thread):
    _hub, connect = online
    device, session = connect(caps={"choice": CHOICE})
    asked = _ask(loop_thread, session, "Pick", ["a", "b"])
    device.next("choice")
    with pytest.raises(gadget_hub.ActionError, match="already showing"):
        loop_thread.run(session.ask_choice("Other", ["c", "d"]))
    device.close()
    with pytest.raises(gadget_hub.ActionError, match="disconnected"):
        asked.result(5)


def test_a_choice_is_not_held_while_the_screen_is_dark_and_questions_still_work(online, loop_thread):
    hub, connect = online
    device, session = connect(caps={"choice": CHOICE}, sleeping=True)
    asked = _ask(loop_thread, session, "Pick", ["a", "b"])
    choice = device.next("choice")
    # Hermes's own yes/no question goes out meanwhile, and its answer reaches the delegate as before.
    answers = []

    async def on_prompt_reply(_session, prompt_id, yes):
        answers.append((prompt_id, yes))

    hub.delegate.on_prompt_reply = on_prompt_reply
    loop_thread.run(session.ask("q1", "Allow command?", "rm -rf build", ttl_s=300))
    prompt = device.next("prompt")
    assert prompt == {"type": "prompt", "id": "q1", "title": "Allow command?", "text": "rm -rf build", "ttl_s": 300}
    device.send("prompt.reply", id="q1", answer="no")
    assert _wait(lambda: answers == [("q1", False)])
    assert not asked.done()
    device.send("choice.reply", id=choice["id"], index=0)
    assert asked.result(5) == 0


# -- the agent tools ------------------------------------------------------------------


@pytest.fixture
def tool(online, loop_thread, monkeypatch):
    """The tools against one real session: everything but Hermes's own session lookup."""
    _hub, connect = online

    def make(**kw):
        device, session = connect(**kw)
        session.paired = True
        monkeypatch.setattr(tools, "_adapter", lambda: object())
        monkeypatch.setattr(tools, "_resolve", lambda _adapter, _ref: (session, None))

        def run(_adapter, coro, timeout=tools.CALL_TIMEOUT_S):
            future = asyncio.run_coroutine_threadsafe(coro, loop_thread.loop)
            try:
                return future.result(timeout)
            except BaseException:
                future.cancel()
                raise

        monkeypatch.setattr(tools, "_run", run)
        return device, session

    return make


def test_gadget_display_sends_a_shaped_list_progress_and_code(tool):
    device, _session = tool(caps={"card": CARD}, cols=12)
    result = json.loads(tools.gadget_display({
        "title": "**Shopping**", "text": "For *tonight*", "seconds": 0, "id": "shop", "progress": "55",
        "items": ["**Milk** — 2 l", "Crème\nfraîche", "🙂", "A very long item that runs off the screen"],
        "qr": "https://example.org/list?a=1"}))
    assert result["success"] is True and "note" not in result
    assert device.frames() == [{
        "type": "display", "title": "Shopping", "body": "For tonight", "ttl_s": 0, "id": "shop", "progress": 55,
        # Markdown stripped, folded to the charset, one line each, cut to the device's line less the bullet's two columns; the emoji-only row is gone.
        "items": ["Milk - 2 l", "Crème fraî", "A very lon"],
        "qr": "https://example.org/list?a=1"}]  # a code holds exactly what was asked for


def test_gadget_display_keeps_plain_cards_as_they_were(tool):
    device, _session = tool(caps={"card": CARD})
    result = json.loads(tools.gadget_display({"title": "Note", "text": "Hello", "seconds": 30}))
    assert result == {"success": True, "device": "watch", "shown_for_s": 30}
    assert device.frames() == [{"type": "display", "title": "Note", "body": "Hello", "ttl_s": 30}]
    assert json.loads(tools.gadget_display({}))["success"] is False
    assert json.loads(tools.gadget_display({"items": "milk"}))["success"] is False
    assert json.loads(tools.gadget_display({"text": "x", "progress": "half"}))["success"] is False
    assert json.loads(tools.gadget_display({"qr": "a" * 288}))["success"] is False
    assert device.frames() == []


def test_gadget_display_says_when_the_content_went_out_as_text(tool):
    device, _session = tool()
    result = json.loads(tools.gadget_display({"title": "Build", "items": ["core", "sim"], "progress": 100}))
    assert result["success"] is True and "plain cards only" in result["note"]
    assert device.frames()[0]["body"] == "[##########] 100%\n* core\n* sim"
    device, _session = tool(caps={"card": {"v": 1, "items": 12, "qr": 14}})
    result = json.loads(tools.gadget_display({"title": "Join", "qr": "https://example.org/too-long"}))
    assert "too long" in result["note"] and "14 bytes" in result["note"]
    assert "qr" not in device.frames()[0]


def test_gadget_display_counts_the_rows_the_device_keeps(tool):
    device, _session = tool(caps={"card": {"v": 1, "items": 4}})
    result = json.loads(tools.gadget_display({"title": "List", "items": list("abcdef")}))
    assert result["items_shown"] == 4
    assert device.frames()[0]["items"] == ["a", "b", "c", "d"]
    device, _session = tool(caps={"card": CARD})
    result = json.loads(tools.gadget_display({"title": "List", "items": [str(n) for n in range(14)]}))
    assert result["items_shown"] == 12
    assert "items_shown" not in json.loads(tools.gadget_display({"title": "List", "items": ["a"]}))


def test_gadget_display_reports_a_structured_card_held_in_the_dark(tool):
    device, _session = tool(caps={"card": CARD}, sleeping=True)
    result = json.loads(tools.gadget_display({"title": "Build", "progress": 5, "id": "b"}))
    assert result["success"] is True and "held" in result
    assert device.frames() == []


def test_gadget_ask_returns_the_option_as_the_agent_wrote_it(tool):
    device, _session = tool(caps={"choice": CHOICE}, cols=10)
    answer = {}
    asked = threading.Thread(target=lambda: answer.update(json.loads(tools.gadget_ask(
        {"question": "Which **list**?", "options": ["Groceries — today", "Hardware store"], "seconds": 30}))))
    asked.start()
    choice = device.next("choice")
    assert choice["title"] == "Which list?"
    assert choice["options"] == ["Grocerie", "Hardware"] and choice["ttl_s"] == 30  # 10 columns, less the bullet's 2
    device.send("choice.reply", id=choice["id"], index=1)
    asked.join(5)
    assert answer == {"success": True, "device": "watch", "answer": "Hardware store", "index": 1,
                      "options_shown": ["Grocerie", "Hardware"],
                      "note": "some options were cut to fit the screen; options_shown is what the user saw"}


def _asked(device, args: dict, index: int = 0) -> tuple[dict, dict]:
    """Runs gadget_ask, answers it, and returns what the device got and what the agent got."""
    answer = {}
    asked = threading.Thread(target=lambda: answer.update(json.loads(tools.gadget_ask(args))))
    asked.start()
    choice = device.next("choice")
    device.send("choice.reply", id=choice["id"], index=index)
    asked.join(5)
    return choice, answer


def test_gadget_ask_sends_a_long_question_whole_where_it_fits(tool):
    device, _session = tool(caps={"choice": {**CHOICE, "title_cols": 20, "title_lines": 3}})
    question = "Which of these lists should I open for you now?"  # 3 lines of 20
    choice, answer = _asked(device, {"question": question, "options": ["a", "b"]})
    assert choice["title"] == question
    assert answer == {"success": True, "device": "watch", "answer": "a", "index": 0}


def test_gadget_ask_cuts_a_question_too_long_for_the_screen_and_says_so(tool):
    device, _session = tool(caps={"choice": {**CHOICE, "title_cols": 20, "title_lines": 2}})
    question = "Which of these lists should I open for you now?"
    choice, answer = _asked(device, {"question": question, "options": ["a", "b"]})
    assert choice["title"] == "Which of these lists should I open for..."
    assert textfmt.wrap(choice["title"], 20) == ["Which of these lists", "should I open for..."]
    assert answer["answer"] == "a" and answer["question_shown"] == choice["title"]
    assert "question was cut" in answer["note"]
    # A device that says nothing about its question's room gets it whole, with nothing to report.
    other, _session = tool(caps={"choice": CHOICE})
    choice, answer = _asked(other, {"question": question, "options": ["a", "b"]})
    assert choice["title"] == question and "note" not in answer


def test_gadget_ask_reports_a_cancel_and_a_timeout(tool, monkeypatch):
    device, _session = tool(caps={"choice": CHOICE})
    answer = {}
    asked = threading.Thread(target=lambda: answer.update(json.loads(tools.gadget_ask(
        {"question": "Pick", "options": ["a", "b"]}))))
    asked.start()
    device.send("choice.reply", id=device.next("choice")["id"], cancelled=True)
    asked.join(5)
    assert answer == {"success": True, "device": "watch", "cancelled": True}

    monkeypatch.setattr(tools, "ASK_MAX_S", 1)  # the wait is 5 s at least; the hub's is what ends it here
    original = gadget_hub.DeviceSession.ask_choice
    monkeypatch.setattr(gadget_hub.DeviceSession, "ask_choice",
                        lambda self, title, options, timeout: original(self, title, options, timeout=0.3))
    late = json.loads(tools.gadget_ask({"question": "Pick", "options": ["a", "b"]}))
    assert late["success"] is False and "no answer" in late["error"]
    assert device.next("choice.close")


def test_gadget_ask_falls_back_to_a_numbered_card_without_the_capability(tool):
    device, _session = tool()
    result = json.loads(tools.gadget_ask({"question": "Which list?", "options": ["Groceries", "Hardware"]}))
    assert result["success"] is False and result["shown_as_card"] is True
    assert "say their answer" in result["error"]
    assert device.frames() == [{"type": "display", "title": "Which list?", "ttl_s": 60,
                                "body": "1. Groceries\n2. Hardware"}]


def test_gadget_ask_checks_its_options(tool):
    device, _session = tool(caps={"choice": {"v": 1, "options": 3}})
    for options in (None, "a,b", ["only"], list("abcdefg")):
        assert "2 to 6" in json.loads(tools.gadget_ask({"question": "Pick", "options": options}))["error"]
    assert "can draw" in json.loads(tools.gadget_ask({"question": "Pick", "options": ["ok", "🙂"]}))["error"]
    assert "at most 3" in json.loads(tools.gadget_ask({"question": "Pick", "options": list("abcd")}))["error"]
    assert device.frames() == []


def test_the_tools_are_declared_where_hermes_looks():
    from pathlib import Path

    manifest = (Path(tools.__file__).parent / "plugin.yaml").read_text(encoding="utf-8")
    for schema in (tools.DEVICES_SCHEMA, tools.DISPLAY_SCHEMA, tools.ASK_SCHEMA, tools.ACTION_SCHEMA):
        assert f"  - {schema['name']}\n" in manifest


def test_a_list_row_is_one_shaped_line():
    assert textfmt.line("**Milk**\n2 l", "ascii") == "Milk 2 l"
    assert textfmt.line("Crème fraîche", "ascii", 8) == "Creme fr"
    assert textfmt.line("Crème fraîche", "latin1", 3) == "Crème fraîche"  # no usable width: left whole
    assert textfmt.line("🙂", "latin1", 20) == ""


def test_wrap_breaks_lines_as_the_device_does():
    assert textfmt.wrap("", 10) == []
    assert textfmt.wrap("one two three", 7) == ["one two", "three"]
    assert textfmt.wrap("abcdefghij kl", 4) == ["abcd", "efgh", "ij", "kl"]  # a long word is cut at the width
    assert textfmt.wrap("日本語 ab", 4) == ["日本", "語", "ab"]  # wide characters take two columns
    assert textfmt.clip("one two three", 7, 2) == ("one two three", False)
    assert textfmt.clip("one two three four", 7, 2) == ("one two thre...", True)  # "one two" / "thre..."
    assert textfmt.wrap("one two thre...", 7) == ["one two", "thre..."]


def test_the_plain_body_of_a_structured_card():
    assert gadget_hub.plain_card_body("") == ""
    assert gadget_hub.plain_card_body("Body", ["a"], 0, "code") == "[----------] 0%\nBody\n* a\ncode"
    assert gadget_hub.plain_card_body("", progress=99) == "[#########-] 99%"


# -- the simulated firmware -----------------------------------------------------------


@requires_sim
@pytest.mark.parametrize("board", ["sim-320x240", "sim-240x135", "sim-466x466-round", "sim-800x480-nospeaker"])
def test_the_simulated_firmware_draws_structured_cards(devserver, make_sim, loop_thread, board):
    hub, _brain, url = devserver()
    sim = make_sim(url, board=board)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert session.card_items == 12 and session.card_qr_bytes >= 42
    loop_thread.run(session.show_card("Build", "Compiling", 0, items=["core", "sim"], progress=40, card_id="b"))
    assert sim.wait_screen("card", timeout=5)
    assert sim.last_received("display")["items"] == ["core", "sim"]
    link = "https://example.org/" + "a" * (session.card_qr_bytes - 20)
    assert session.card_shown_as(qr=link) == "card"
    loop_thread.run(session.show_card("Join", "", 0, qr=link))
    assert sim.wait_for(lambda: (sim.last_received("display") or {}).get("qr") == link, timeout=5)
    assert sim.status()["screen"] == "card"


@requires_sim
def test_the_simulated_firmware_answers_a_choice_with_its_keys(devserver, make_sim, loop_thread):
    hub, _brain, url = devserver()
    sim = make_sim(url)
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    assert session.choice_options == 6
    asked = _ask(loop_thread, session, "Which list?", ["Groceries", "Hardware", "Pharmacy"], timeout=30)
    assert sim.wait_screen("choice", timeout=5)
    sim.run_for(0.7)  # presses this soon after it appears answer nothing
    sim.tap("down")
    sim.tap("down")
    sim.tap("talk")
    assert sim.wait_for(asked.done, timeout=5)
    assert asked.result() == 2
    assert sim.status()["screen"] != "choice"

    cancelled = _ask(loop_thread, session, "Again?", ["Yes", "No"], timeout=30)
    assert sim.wait_screen("choice", timeout=5)
    sim.run_for(0.7)
    sim.tap("cancel")
    assert sim.wait_for(cancelled.done, timeout=5)
    assert cancelled.result() is None


@requires_sim
def test_the_simulated_firmware_answers_a_choice_by_touch_and_drops_one_that_timed_out(
        devserver, make_sim, loop_thread):
    hub, _brain, url = devserver()
    sim = make_sim(url, board="sim-466x466-round")
    assert sim.wait_screen("ready", timeout=10)
    session = next(iter(hub.sessions.values()))
    asked = _ask(loop_thread, session, "Pick", ["One", "Two"], timeout=30)
    assert sim.wait_screen("choice", timeout=5)
    sim.run_for(0.7)
    assert sim.console("choose 2") == "@ok choose"
    assert sim.wait_for(asked.done, timeout=5)
    assert asked.result() == 1

    late = _ask(loop_thread, session, "Pick", ["One", "Two"], timeout=0.5)
    assert sim.wait_screen("choice", timeout=5)
    assert sim.wait_for(late.done, timeout=5)
    with pytest.raises(gadget_hub.ActionError):
        late.result()
    assert sim.wait_for(lambda: sim.status()["screen"] != "choice", timeout=5)  # choice.close took it down


def test_a_row_is_cut_by_the_columns_it_takes_on_the_screen():
    from hermes_gadget_plugin import textfmt

    wide = "中文" * 10  # 20 characters, two columns each
    assert textfmt.line(wide, "cjk", 23) == wide[:11]  # 22 columns: the device would cut the rest unseen
    assert textfmt.line("abc " + wide, "cjk", 10) == "abc " + wide[:3]
    assert textfmt.line("A very long item that runs off", "ascii", 10) == "A very lon"


def test_a_codes_text_keeps_its_spaces(tool):
    device, _session = tool(caps={"card": CARD})
    assert json.loads(tools.gadget_display({"title": "Wi-Fi", "qr": " pass word "}))["success"] is True
    assert device.frames()[0]["qr"] == " pass word "
    assert json.loads(tools.gadget_display({"qr": "   "}))["success"] is False  # nothing to show
