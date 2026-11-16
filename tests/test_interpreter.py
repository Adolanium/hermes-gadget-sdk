"""Interpreter mode: a gadget translates between two languages instead of answering.

Three layers, each with what it needs:

* the plugin's own logic (``plugin/interpret.py``, the desk request, the CLI) runs anywhere;
* a simulated device, the production core, holds a two-language exchange against the production
  hub, with ``tests/fakes/fake_interpreter.py`` standing in for Hermes's speech-to-text and agent;
* the adapter on Hermes's real classes: which chat a message goes to, with what instruction, and
  the language each recording is transcribed in (skipped without a Hermes checkout).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import struct
import threading
import time

import pytest

from conftest import requires_hermes, requires_sim
from fakes.fake_interpreter import FakeInterpreter
from hermes_gadget_plugin import cli as plugin_cli
from hermes_gadget_plugin import interpret, runtime
from hermes_gadget_plugin.desk import Desk
from hermes_gadget_plugin.store import DeviceStore

DEVICE = "hg-0123456789abcdef"


def _tone(seconds: float, rate: int = 16000) -> bytes:
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 330 * i / rate)))
                    for i in range(int(rate * seconds)))


# -- the plugin's own logic ---------------------------------------------------------------------------


def test_languages_are_codes_or_english_names():
    assert interpret.language("es") == "es"
    assert interpret.language(" PT-BR ") == "pt-BR"
    assert interpret.language("Spanish") == "es" and interpret.language("japanese") == "ja"
    for bad in ("", None, "klingonese", "e", "en; rm -rf", "12"):
        assert interpret.language(bad) is None, bad


def test_a_session_needs_two_different_languages():
    session = interpret.start("Spanish", "en")
    assert (session.a, session.b, session.speaker, session.auto) == ("es", "en", "es", False)
    with pytest.raises(ValueError, match="two different languages"):
        interpret.start("es", "Spanish")
    with pytest.raises(ValueError, match="'elvish' is not a language"):
        interpret.start("es", "elvish")


def test_the_speakers_take_turns_and_each_recording_has_its_language():
    session = interpret.start("es", "en")
    assert (session.stt_language, session.target, session.label) == ("es", "en", "ES > EN")
    assert session.message() == {"type": "interpret", "on": True, "label": "ES > EN", "flip": True}
    session.flip()
    assert (session.stt_language, session.target, session.label) == ("en", "es", "EN > ES")
    session.flip()
    assert session.stt_language == "es"
    assert session.describe() == {"languages": ["es", "en"], "detect": "turns", "next_speaker": "es"}


def test_with_auto_the_provider_detects_the_language_and_nobody_takes_turns():
    session = interpret.start("es", "en", auto=True)
    assert session.stt_language is None  # nothing is fixed: the STT provider's own detection applies
    assert session.message() == {"type": "interpret", "on": True, "label": "ES <> EN", "flip": False}
    assert session.describe() == {"languages": ["es", "en"], "detect": "auto"}


def test_the_instruction_asks_for_a_translation_and_nothing_else():
    session = interpret.start("es", "pt-BR")
    prompt = interpret.instruction(session)
    assert "between Spanish and Portuguese (pt-BR)" in prompt
    assert "transcribed as Spanish: reply in Portuguese (pt-BR)" in prompt
    assert "Never answer" in prompt and "no tools" in prompt and "nothing to memory" in prompt
    session.flip()
    assert "transcribed as Portuguese (pt-BR): reply in Spanish" in interpret.instruction(session)
    # With auto the direction is the model's to find: nothing names this message's language.
    assert "transcribed as" not in interpret.instruction(interpret.start("es", "en", auto=True))
    # A code without a name here still reaches the model as a code.
    assert "the language with code xx" in interpret.instruction(interpret.start("xx", "en"))


def test_translations_have_a_chat_of_their_own():
    chat = interpret.chat_id(DEVICE)
    assert chat != DEVICE and chat.startswith(DEVICE)
    assert interpret.is_chat(chat)
    # Neither the device's conversation nor its chat with another profile (profile_mode: virtual).
    assert not interpret.is_chat(DEVICE) and not interpret.is_chat(f"{DEVICE}~interpret")
    assert not interpret.is_chat(None)


def test_a_translation_the_font_cannot_draw_is_announced_as_spoken_only():
    # Latin text folds to what the charset draws, as every reply does.
    assert interpret.shown("¿Dónde está?", "latin1", "es") == "¿Dónde está?"
    assert interpret.shown("¿Dónde está?", "ascii", "es") == "Donde esta?"
    assert interpret.shown("Где вокзал?", "latin1-cyrillic", "ru") == "Где вокзал?"
    assert interpret.shown("駅はどこですか", "cjk", "ja") == "駅はどこですか"
    # A script the font lacks would be dropped, leaving nothing or scraps: say so instead.
    for text, charset, target, language in (
            ("駅はどこですか", "latin1-cyrillic", "ja", "Japanese"),
            ("Где вокзал?", "latin1", "ru", "Russian"),
            ("أين المحطة؟", "cjk", "ar", "Arabic"),
            ("Station はどこですか、教えてください", "latin1", "ja", "Japanese")):
        assert interpret.shown(text, charset, target) == f"({language}: spoken only. This screen can't show its script.)"
    assert interpret.shown("駅", "latin1", None).startswith("(Translation: spoken only.")
    assert interpret.shown("", "latin1", "ja") == ""


def test_the_desk_takes_interpreter_requests_and_refuses_bad_languages(tmp_path):
    desk = Desk(tmp_path)
    desk.post(DEVICE, "interpret", a="Spanish", b="en")
    desk.post(DEVICE, "interpret", a="es", b="en", auto=True)
    desk.post(DEVICE, "interpret", off=True)
    requests = [{k: v for k, v in req.items() if k != "at"} for _id, req in desk.pending()]
    assert {"device": DEVICE, "kind": "interpret", "a": "es", "b": "en", "auto": False} in requests
    assert {"device": DEVICE, "kind": "interpret", "a": "es", "b": "en", "auto": True} in requests
    assert {"device": DEVICE, "kind": "interpret", "off": True} in requests
    with pytest.raises(ValueError, match="not a language"):
        desk.post(DEVICE, "interpret", a="es", b="gibberish!")
    with pytest.raises(ValueError, match="two different"):
        desk.post(DEVICE, "interpret", a="es", b="es")


def _gateway_answers(directory, reply):
    desk, seen = Desk(directory), []

    def run():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for req_id, req in desk.pending():
                seen.append(req)
                desk.finish(req_id, reply(req))
                return
            time.sleep(0.02)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, seen


def test_hermes_gadget_interpret_starts_and_stops_the_mode(tmp_path, monkeypatch, capsys):
    store = DeviceStore(tmp_path)
    store.enroll(DEVICE, b"k" * 32, name="Kitchen", board="test", address="127.0.0.1")
    monkeypatch.setattr(plugin_cli, "_store", lambda: DeviceStore(tmp_path))

    def run(*languages, auto=False):
        plugin_cli.handle(argparse.Namespace(gadget_command="interpret", device="kitchen",
                                             languages=list(languages), auto=auto))

    thread, seen = _gateway_answers(tmp_path, lambda req: {"ok": True, "label": "ES > EN", "draws": True})
    run("spanish", "en")
    thread.join()
    assert (seen[0]["a"], seen[0]["b"], seen[0]["auto"]) == ("es", "en", False)
    out = capsys.readouterr().out
    assert "Kitchen is interpreting: ES > EN" in out and "take turns" in out
    assert "hermes gadget interpret kitchen off" in out

    # Firmware from before the mode still interprets; the command says what it lacks.
    thread, seen = _gateway_answers(tmp_path, lambda req: {"ok": True, "label": "ES <> EN", "draws": False})
    run("es", "en", auto=True)
    thread.join()
    assert seen[0]["auto"] is True
    out = capsys.readouterr().out
    assert "detects the language" in out and "doesn't draw the interpreter screen" in out

    thread, seen = _gateway_answers(tmp_path, lambda req: {"ok": True, "was_on": True})
    run("off")
    thread.join()
    assert seen[0]["off"] is True
    assert "no longer interpreting" in capsys.readouterr().out

    # Bad input never reaches the gateway.
    with pytest.raises(SystemExit, match="Give two languages"):
        run("es")
    with pytest.raises(SystemExit, match="'elvish' is not a language"):
        run("es", "elvish")
    assert Desk(tmp_path).pending() == []
    # No gateway: a message, not a hang.
    monkeypatch.setattr(plugin_cli, "INTERPRET_TIMEOUT_S", 0.3)
    with pytest.raises(SystemExit, match="gateway didn't answer"):
        run("es", "en")


# -- a simulated device against the hub, Hermes's part played by the fake -----------------------------


def _interpreting_sim(devserver, make_sim, loop_thread, first="es", second="en", **kw):
    hub, brain, url = devserver()
    fake = FakeInterpreter().install(brain)
    sim = make_sim(url, **kw)
    assert sim.wait_screen("ready", timeout=10)
    session = hub.get(sim.status()["device_id"])
    loop_thread.run(brain.interpret(session, first, second))
    return sim, session, brain, fake


def _says(sim, expected_reply: str) -> None:
    """One person holds TALK, speaks, and the device shows and plays the translation."""
    sim.speak_pcm(_tone(1.0), tail_s=0.2)
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == expected_reply, timeout=10), \
        sim.last_received("reply")
    assert sim.wait_screen("ready", timeout=10)  # the spoken translation has finished playing


@requires_sim
def test_two_people_talk_through_the_gadget_in_two_languages(devserver, make_sim, loop_thread, tmp_path):
    sim, session, brain, fake = _interpreting_sim(devserver, make_sim, loop_thread)
    assert session.caps["interpret"] == 1  # the firmware draws the mode
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    sim.screenshot(tmp_path / "1-ready.png")

    # The Spanish speaker asks; the recording is transcribed in Spanish and comes back in English.
    _says(sim, "Where is the station?")
    assert sim.last_received("transcript")["text"] == "¿Dónde está la estación?"
    assert sim.last_received("audio.start") is not None  # spoken, not only shown
    # The gadget changes hands: the next recording is the English speaker's.
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)
    sim.screenshot(tmp_path / "2-english.png")

    _says(sim, "Siga recto y gire a la izquierda.")
    assert sim.last_received("transcript")["text"] == "Go straight and turn left."
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    sim.screenshot(tmp_path / "3-spanish.png")

    _says(sim, "Thank you very much.")
    _says(sim, "De nada.")
    # Each recording was transcribed in the language whose turn it was.
    assert fake.transcribed == ["es", "en", "es", "en"]
    assert [label for _text, label in fake.translated] == ["ES > EN", "EN > ES", "ES > EN", "EN > ES"]
    assert all((tmp_path / name).stat().st_size > 0 for name in ("1-ready.png", "2-english.png", "3-spanish.png"))


@requires_sim
def test_a_short_cancel_press_lets_the_same_person_speak_again(devserver, make_sim, loop_thread):
    sim, _session, _brain, fake = _interpreting_sim(devserver, make_sim, loop_thread)
    _says(sim, "Where is the station?")
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)
    sim.tap("cancel")  # the Spanish speaker has more to say
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    assert sim.device.screen() == "ready"  # the press switched the speaker; no dashboard opened
    _says(sim, "Thank you very much.")
    assert fake.transcribed == ["es", "es"]


@requires_sim
def test_typed_text_is_translated_too(devserver, make_sim, loop_thread):
    sim, _session, _brain, fake = _interpreting_sim(devserver, make_sim, loop_thread)
    sim.type_text("buenos días")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "good morning", timeout=10)
    assert fake.transcribed == []  # nothing to transcribe
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)


@requires_sim
def test_holding_cancel_leaves_the_mode_and_the_device_answers_again(devserver, make_sim, loop_thread):
    sim, session, brain, _fake = _interpreting_sim(devserver, make_sim, loop_thread)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    sim.press("cancel")
    assert sim.wait_for(lambda: any(m.get("type") == "session.new" for m in sim.sent), timeout=5)
    sim.release("cancel")
    assert sim.wait_for(lambda: "interpret" not in sim.status(), timeout=5)
    assert brain.interpreting == {}
    assert sim.last_received("notice")["text"] == "Interpreter off"
    assert sim.wait_screen("ready", timeout=5)
    sim.type_text("ping")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "You said: ping", timeout=10)
    # Outside the mode the hold starts a new conversation, as it always did.
    assert session.device_id not in brain.interpreting


@requires_sim
def test_a_script_the_screen_cannot_draw_is_still_spoken(devserver, make_sim, loop_thread):
    sim, session, _brain, fake = _interpreting_sim(devserver, make_sim, loop_thread, first="en", second="ja")
    assert session.charset == "latin1-cyrillic"  # this build's font has no kana or kanji
    sim.type_text("Where is the station?")
    assert sim.wait_for(lambda: sim.last_received("reply") is not None
                        and not sim.last_received("reply").get("interim"), timeout=10)
    assert sim.last_received("reply")["text"] == "(Japanese: spoken only. This screen can't show its script.)"
    assert sim.last_received("audio.start") is not None
    assert fake.translated == [("Where is the station?", "EN > JA")]


# -- the adapter on Hermes's classes ------------------------------------------------------------------

hermes = pytest.mark.usefixtures("registered_platform")


def _interpreting_gadget(gadget, make_sim, first="es", second="en", auto=False, **kw):
    from hermes_fixtures import _paired_sim

    sim = _paired_sim(gadget, make_sim, **kw)
    device_id = sim.status()["device_id"]
    session = gadget.adapter.hub.get(device_id)
    gadget.run(gadget.adapter.interpret_start(session, first, second, auto))
    gadget.events.clear()
    return sim, device_id, session


def _voice_events(gadget):
    return [e for e in gadget.events if e.message_type.value == "voice"]


def _speaks(gadget, sim, count: int):
    sim.speak_pcm(_tone(1.0), tail_s=0.2)
    assert sim.wait_for(lambda: len(_voice_events(gadget)) == count, timeout=10)
    assert sim.wait_for(lambda: (sim.last_received("turn.end") or {}).get("outcome") == "success", timeout=10)
    return _voice_events(gadget)[-1]


@requires_hermes
@requires_sim
@hermes
def test_the_adapter_sends_speech_to_the_interpreter_chat_in_the_speakers_language(gadget, make_sim):
    sim, device_id, _session = _interpreting_gadget(gadget, make_sim, name="Kitchen")
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)

    first = _speaks(gadget, sim, 1)
    # Its own chat, the same device as the sender: Hermes keeps a separate session for it.
    assert first.source.chat_id == interpret.chat_id(device_id) != device_id
    assert first.source.user_id == device_id and first.source.chat_type == "dm"
    # The agent is told to translate, not handed the device's context and actions.
    assert "interpreter between Spanish and English" in first.channel_prompt
    assert "transcribed as Spanish: reply in English" in first.channel_prompt
    assert "Kitchen" not in first.channel_prompt and "gadget_action" not in first.channel_prompt
    assert runtime.clip_language(first.media_urls[0]) == "es"
    # The reply to that chat reaches the device, and the turn's end hands the gadget over.
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "echo:", timeout=10)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)
    assert sim.wait_screen("ready", timeout=10)

    second = _speaks(gadget, sim, 2)
    assert second.source.chat_id == interpret.chat_id(device_id)
    assert "transcribed as English: reply in Spanish" in second.channel_prompt
    assert runtime.clip_language(second.media_urls[0]) == "en"
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)


@requires_hermes
@requires_sim
@hermes
def test_the_devices_conversation_gets_none_of_it_and_is_back_afterwards(gadget, make_sim):
    sim, device_id, session = _interpreting_gadget(gadget, make_sim, name="Kitchen")
    sim.type_text("hola")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "echo: hola", timeout=10)
    assert [e.source.chat_id for e in gadget.events] == [interpret.chat_id(device_id)]
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)

    assert gadget.run(gadget.adapter.interpret_stop(session)) is True
    assert gadget.run(gadget.adapter.interpret_stop(session)) is False
    assert sim.wait_for(lambda: "interpret" not in sim.status(), timeout=5)
    gadget.events.clear()
    sim.type_text("what's up")
    assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == "echo: what's up", timeout=10)
    event = gadget.events[-1]
    assert event.source.chat_id == device_id
    assert "Kitchen" in event.channel_prompt and "interpreter" not in event.channel_prompt


@requires_hermes
@requires_sim
@hermes
def test_holding_cancel_leaves_interpreter_mode_instead_of_starting_a_new_session(gadget, make_sim):
    sim, device_id, _session = _interpreting_gadget(gadget, make_sim)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    sim.console("new-session")
    assert sim.wait_for(lambda: "interpret" not in sim.status(), timeout=5)
    assert gadget.adapter.interpreting(device_id) is None
    assert sim.wait_screen("ready", timeout=5)
    sim.run_for(0.3)
    assert not any(e.text == "/new" for e in gadget.events)  # the conversation was not reset
    # The next hold is an ordinary one.
    sim.console("new-session")
    assert sim.wait_for(lambda: any(e.text == "/new" and e.source.chat_id == device_id for e in gadget.events),
                        timeout=5)


@requires_hermes
@requires_sim
@hermes
def test_a_cancel_press_switches_the_speaker_and_a_reconnect_keeps_the_mode(gadget, make_sim):
    sim, device_id, _session = _interpreting_gadget(gadget, make_sim)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES > EN", timeout=5)
    sim.tap("cancel")
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)
    assert gadget.adapter.interpreting(device_id).stt_language == "en"
    sim.console("reconnect")
    assert sim.wait_screen("connecting", timeout=5)
    assert sim.wait_screen("ready", timeout=10)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "EN > ES", timeout=5)


@requires_hermes
@requires_sim
@hermes
def test_with_auto_no_language_is_fixed_not_even_the_configured_one(gadget, make_sim):
    gadget.adapter._stt_language = "pt"  # platforms.gadget.extra.stt_language
    sim, device_id, session = _interpreting_gadget(gadget, make_sim, auto=True)
    assert sim.wait_for(lambda: sim.status().get("interpret") == "ES <> EN", timeout=5)
    event = _speaks(gadget, sim, 1)
    assert event.source.chat_id == interpret.chat_id(device_id)
    assert runtime.clip_language(event.media_urls[0]) is None
    assert "transcribed as" not in event.channel_prompt
    sim.run_for(0.3)
    assert sim.status().get("interpret") == "ES <> EN"  # nobody takes turns
    assert sim.wait_screen("ready", timeout=10)
    # Outside the mode the configured language applies again.
    gadget.run(gadget.adapter.interpret_stop(session))
    event = _speaks(gadget, sim, 2)
    assert event.source.chat_id == device_id
    assert runtime.clip_language(event.media_urls[0]) == "pt"


@requires_hermes
@requires_sim
@hermes
def test_a_translation_the_device_cannot_draw_is_announced_and_only_in_the_interpreter_chat(gadget, make_sim):
    sim, device_id, _session = _interpreting_gadget(gadget, make_sim, first="en", second="ja")
    gadget.run(gadget.adapter.send(interpret.chat_id(device_id), "駅はどこですか"))
    assert sim.wait_for(lambda: sim.last_received("reply") is not None, timeout=5)
    assert sim.last_received("reply")["text"] == "(Japanese: spoken only. This screen can't show its script.)"
    # An ordinary reply in the device's own chat folds as it always did.
    gadget.run(gadget.adapter.send(device_id, "駅 is the word for station"))
    assert sim.wait_for(lambda: sim.last_received("reply")["text"] == "is the word for station", timeout=5)


@requires_hermes
@requires_sim
@hermes
def test_the_agent_tool_starts_and_stops_the_mode(gadget, make_sim, monkeypatch):
    from gateway import session_context
    from hermes_fixtures import _paired_sim
    from hermes_gadget_plugin import tools

    sim = _paired_sim(gadget, make_sim, name="Desk")
    device_id = sim.status()["device_id"]
    monkeypatch.setattr(session_context, "get_session_env",
                        lambda name, default="": {"HERMES_SESSION_PLATFORM": "gadget",
                                                  "HERMES_SESSION_CHAT_ID": device_id}.get(name, default))
    pool = concurrent.futures.ThreadPoolExecutor(1)  # tools run on agent worker threads

    def call(args):
        fut = pool.submit(tools.gadget_interpret, args)
        assert sim.wait_for(fut.done, timeout=15)
        return json.loads(fut.result())

    started = call({"languages": ["French", "de"]})
    assert started["success"] and started["interpreting"] is True
    assert (started["languages"], started["detect"], started["next_speaker"]) == (["fr", "de"], "turns", "fr")
    assert sim.wait_for(lambda: sim.status().get("interpret") == "FR > DE", timeout=5)
    assert call({"languages": ["fr", "de"], "detect": "auto"})["detect"] == "auto"
    assert sim.wait_for(lambda: sim.status().get("interpret") == "FR <> DE", timeout=5)

    for bad, why in (({"languages": ["fr"]}, "two languages"), ({"languages": ["fr", "elvish"]}, "not a language"),
                     ({}, "two languages")):
        refused = call(bad)
        assert not refused["success"] and why in refused["error"], refused
    assert gadget.adapter.interpreting(device_id).label == "FR <> DE"  # a refused call changes nothing

    stopped = call({"stop": True})
    assert stopped == {"success": True, "device": "Desk", "interpreting": False, "was_on": True}
    assert sim.wait_for(lambda: "interpret" not in sim.status(), timeout=5)
    assert call({"stop": True})["was_on"] is False
    pool.shutdown()
