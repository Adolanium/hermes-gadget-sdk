"""Desktop controls against the real simulator and development hub."""

import gc
import os
import sys
import time
import wave
from types import SimpleNamespace

import pytest

from conftest import requires_sim
from hermes_gadget.sim import Simulator
from hermes_gadget.sim.audio_io import Microphone, Speaker
from hermes_gadget.sim.conversation import Conversation
from hermes_gadget.sim.window import SimulatorWindow


def test_conversation_streams_transcripts_and_replaces_cumulative_deltas():
    chat = Conversation()
    chat.receive("sent", {"type":"audio.end"})
    chat.receive("received", {"type":"turn.start", "turn":"one"})
    chat.receive("received", {"type":"transcript", "text":"Hello"})
    chat.receive("received", {"type":"reply.delta", "turn":"one", "text":"Good"})
    chat.receive("received", {"type":"reply.delta", "turn":"one", "text":"Good morning"})
    chat.receive("received", {"type":"reply", "turn":"one", "text":"Good morning!"})
    chat.receive("received", {"type":"reply", "turn":"old", "text":"stale"})
    assert [(m.role, m.text) for m in chat.messages] == [("You", "Hello"), ("Hermes", "Good morning!")]
    chat.receive("sent", {"type":"session.new"})
    chat.receive("sent", {"type":"text", "text":"A fresh start"})
    assert [(m.role, m.text) for m in chat.messages] == [("You", "A fresh start")]


def test_audio_failure_is_visible_and_controls_can_disable_it(monkeypatch):
    def unavailable(**_):
        raise OSError("Microphone permission denied")
    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(RawInputStream=unavailable, RawOutputStream=unavailable))
    mic = Microphone()
    mic.set_live(True)
    mic.start(16000)
    assert mic.error == "Microphone permission denied"
    with pytest.raises(RuntimeError, match="Finish the recording"):
        mic.set_live(False)
    mic.stop()
    mic.set_live(False)
    assert (mic.live, mic.error) == (False, "")
    speaker = Speaker(None)
    speaker.set_live(True)
    speaker.begin(16000)
    assert speaker.error == "Microphone permission denied"
    speaker.set_live(False)
    assert (speaker.live, speaker.error) == (False, "")
    speaker.abort()


@pytest.fixture
def window(tmp_path):
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") and not os.environ.get("HERMES_GADGET_UI_TESTS"):
        pytest.skip("needs a desktop or xvfb-run")
    # Dispose of previous Tk interpreters on the UI thread before starting network workers.
    gc.collect()
    sim = Simulator(state_dir=tmp_path / "device", board="sim-466x466-round")
    window = SimulatorWindow(sim)
    sim.start(network=False)
    window.root.update()
    yield window
    window._quit()


def drive(window, until, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        window.sim.step()
        window._refresh()
        window.root.update()
        if until():
            return
        time.sleep(0.01)
    pytest.fail("simulator window did not reach the expected state")


@requires_sim
def test_window_sends_real_messages_copies_reply_and_opens_developer_tools(window, devserver):
    _, _, url = devserver()
    assert window._apply_settings(url, window.sim.board.name, False, False)
    window.sim.set_network(True)
    drive(window, lambda: window.sim.status()["screen"] == "ready")
    assert window.connection_var.get() == "● Connected · paired"
    window.text_entry.insert(0, "hello desktop")
    window.text_entry.focus_force()
    window.root.update()
    window.text_entry.event_generate("<Return>")
    drive(window, lambda: window.history.latest_reply() == "You said: hello desktop")
    assert [(m.role, m.text) for m in window.history.messages] == [("You", "hello desktop"), ("Hermes", "You said: hello desktop")]
    window.copy_reply.invoke()
    assert window.root.clipboard_get() == "You said: hello desktop"
    window.developer_toggle.invoke()
    window.root.update()
    assert window.developer.winfo_ismapped() == 1
    assert window.talk.winfo_ismapped() == 1
    assert window.talk.winfo_rooty() + window.talk.winfo_height() < window.developer_toggle.winfo_rooty()
    window.console_entry.delete(0, "end")
    window.console_entry.insert(0, "status")
    window.console_entry.focus_force()
    window.root.update()
    window.console_entry.event_generate("<Return>")
    assert "@status" in window.log.get("1.0", "end")
    window.developer_toggle.invoke()
    window.root.update()
    assert window.developer.winfo_ismapped() == 0
    identity = window.sim.status()["device_id"]
    assert window._apply_settings(url, "sim-320x240", False, False)
    drive(window, lambda: window.sim.status()["screen"] == "ready")
    assert window.sim.status()["device_id"] == identity
    window.text_entry.insert(0, "new board")
    window._send_text()
    drive(window, lambda: window.history.latest_reply() == "You said: new board")
    assert window.sim.status()["board"] == "sim-320x240"


@requires_sim
def test_play_last_audio_reaches_the_output_and_a_new_session_clears_it(window, tmp_path, monkeypatch):
    callbacks = []

    class Output:
        def __init__(self, **options):
            callbacks.append(options["callback"])

        def start(self):
            pass

        def stop(self):
            pass

        def close(self):
            pass

    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(RawOutputStream=Output))
    path = tmp_path / "reply.wav"
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(b"\xe8\x03\x18\xfc")  # 1000 and -1000, scaled to the default 70% volume.
    window.sim.speaker.last_file = path
    window._refresh()
    window.replay_reply.invoke()
    output = bytearray(4)
    callbacks[0](output, 2, None, None)
    assert output == b"\xbc\x02\x44\xfd"
    window._message("sent", {"type":"session.new"})
    window._refresh()
    assert window.replay_reply.cget("state") == "disabled"


@requires_sim
def test_offline_text_is_kept_and_board_changes_preserve_identity(window, monkeypatch):
    window.text_entry.insert(0, "keep my draft")
    window._send_text()
    assert window.text_entry.get() == "keep my draft"
    assert "Connect and pair" in window.notice_var.get()
    old_id = window.sim.status()["device_id"]
    assert window._apply_settings("ws://127.0.0.1:9876/gadget", "sim-240x240-nospeaker", False, False)
    window.root.update()
    assert window.sim.status()["device_id"] == old_id
    assert window.text_entry.get() == "keep my draft"
    assert (window.sim.board.width, window.sim.board.speaker) == (240, False)
    assert window._apply_settings("invalid", "sim-320x240", False, False) is False
    assert window.sim.status()["server"] == "ws://127.0.0.1:9876/gadget"
    monkeypatch.setattr(window.root, "winfo_screenheight", lambda: 768)
    monkeypatch.setattr(window.root, "winfo_screenwidth", lambda: 1024)
    assert window._apply_settings("ws://127.0.0.1:9876/gadget", "sim-466x466-round", False, False)
    window.root.update()
    assert window.root.winfo_height() < 768
    assert window.root.winfo_width() < 1024
    assert window.talk.winfo_ismapped() == 1
    assert window.talk.winfo_rooty() + window.talk.winfo_height() < window.developer_toggle.winfo_rooty()


@requires_sim
def test_focus_changes_release_talk_and_typing_does_not_record(window, devserver):
    _, _, url = devserver()
    window._apply_settings(url, window.sim.board.name, False, False)
    window.sim.set_network(True)
    drive(window, lambda: window.sim.status()["screen"] == "ready")
    window._button("talk", True)
    drive(window, lambda: window.sim.mic.active)
    window.text_entry.focus_force()
    window.root.update()
    window._release_if_unfocused()
    assert window.sim.mic.active is False
    assert [m["type"] for m in window.sim.sent if m["type"].startswith("audio.")] == ["audio.start", "audio.cancel"]
    window._key("talk", True)
    assert window.sim.mic.active is False
