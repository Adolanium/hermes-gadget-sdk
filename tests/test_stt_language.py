"""``platforms.gadget.extra.stt_language``: gadget recordings are transcribed in a fixed language.

The pure tests run anywhere. The Hermes ones put the plugin's ``pre_transcription`` hook in front
of Hermes's real STT dispatch, with the backend replaced, and check the language it receives.
"""

from __future__ import annotations

import asyncio
import importlib.util
import types
from pathlib import Path

import pytest

from conftest import REPO, requires_hermes
from hermes_gadget_plugin import runtime, stt


@pytest.fixture(autouse=True)
def no_noted_clips():
    with runtime._lock:
        runtime._stt_clips.clear()
    yield
    with runtime._lock:
        runtime._stt_clips.clear()


class _HookCtx:
    def __init__(self):
        self.hooks = {}

    def register_hook(self, name, callback):
        self.hooks.setdefault(name, []).append(callback)


def test_auto_or_blank_leaves_the_language_to_the_profile():
    assert stt.normalize(None) is None
    assert stt.normalize("") is None
    assert stt.normalize("  AUTO ") is None
    assert stt.normalize(" pt ") == "pt"
    assert stt.normalize("pt-BR") == "pt-BR"
    assert stt.normalize("PT") == "pt"  # the primary code lowercased, as Whisper expects


def test_a_value_that_is_not_a_language_code_keeps_the_profiles_settings(caplog):
    # Passed on, "Portuguese" would make every gadget transcription fail; None keeps the profile's own.
    for bad in ("Portuguese", "pt_BR", "p", "en; rm", "12"):
        assert stt.normalize(bad) is None, bad
    assert "not a language code" in caplog.text


def test_the_plugin_registers_the_pre_transcription_hook():
    ctx = _HookCtx()
    stt.register(ctx)
    assert ctx.hooks == {"pre_transcription": [stt.pre_transcription]}


def test_only_gadget_recordings_get_the_language(tmp_path):
    recording = tmp_path / "cache" / "audio_0123456789ab.wav"
    runtime.note_clip(str(recording), "pt")

    assert stt.pre_transcription(file_path=str(recording), provider="local", model=None,
                                 language=None, prompt=None, source="gateway") == {"language": "pt"}
    # Hermes uploads a silence-trimmed copy to cloud providers, from a temporary directory.
    trimmed = tmp_path / "hermes-stt-trim-x" / "audio_0123456789ab-trimmed.m4a"
    assert stt.pre_transcription(file_path=str(trimmed)) == {"language": "pt"}
    # Another platform's voice note, a near-miss name, or no path: left to the profile.
    assert stt.pre_transcription(file_path=str(tmp_path / "cache" / "audio_ba9876543210.ogg")) is None
    assert stt.pre_transcription(file_path=str(tmp_path / "audio_0123456789abc.wav")) is None
    assert stt.pre_transcription(file_path=None) is None


def test_old_recordings_are_forgotten():
    for i in range(runtime.MAX_STT_CLIPS + 1):
        runtime.note_clip(f"/cache/audio_{i:012d}.wav", "pt")
    assert runtime.clip_language(f"/cache/audio_{0:012d}.wav") is None
    assert runtime.clip_language(f"/cache/audio_{runtime.MAX_STT_CLIPS:012d}.wav") == "pt"


def test_every_profile_copy_of_the_plugin_sees_the_recordings():
    # Hermes imports the plugin once per profile; the answering profile's copy fires the hook,
    # while the default profile's adapter noted the recording.
    spec = importlib.util.spec_from_file_location("hermes_plugins_gadget__home_test.runtime",
                                                  REPO / "plugin" / "runtime.py")
    other_copy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(other_copy)
    runtime.note_clip("/cache/audio_aaaaaaaaaaaa.wav", "pt")
    assert other_copy.clip_language("/cache/audio_aaaaaaaaaaaa.wav") == "pt"


# -- on Hermes's STT dispatch ----------------------------------------------------------------------


def _record(adapter, monkeypatch) -> str:
    """Save one recording through the adapter as a device's utterance; return its cached path."""
    seen = {}

    def event(session, msg_id, text, **kw):
        """Stand-in for ``_event`` that keeps the keyword arguments, media paths included."""
        seen.update(kw)
        return kw

    async def forward(session, event):
        return None

    monkeypatch.setattr(adapter, "_event", event)
    monkeypatch.setattr(adapter, "_forward", forward)
    session = types.SimpleNamespace(device_id="gadget-1")
    from hermes_gadget_plugin.audio import wav_bytes

    asyncio.run(adapter.on_utterance(session, "m1", wav_bytes(b"\0\0" * 1600, 16000), 0.1))
    return seen["media_urls"][0]


def _transcribe(monkeypatch, path: str, stt_config: dict):
    """Run Hermes's STT dispatch for ``path`` with the gadget hook loaded; return the language
    the local whisper backend was given."""
    import hermes_cli.plugins as plugins
    import tools.transcription_tools as tt

    ctx = _HookCtx()
    stt.register(ctx)
    hooks = ctx.hooks
    monkeypatch.setattr(plugins, "has_hook", lambda name: name in hooks)
    monkeypatch.setattr(plugins, "invoke_hook", lambda name, **kw: [
        r for r in (cb(**kw) for cb in hooks.get(name, [])) if r is not None])
    given = {}

    def local(file_path, model_name, language=None, prompt=None):
        """Stand-in for the local whisper backend that records the language it was given."""
        given["language"] = language
        return {"success": True, "transcript": "oi", "provider": "local"}

    monkeypatch.setattr(tt, "_transcribe_local", local)
    result = tt._dispatch_stt_provider(path, "local", stt_config, None, "gateway")
    assert result["success"]
    return given["language"]


def _adapter(**extra):
    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter

    return GadgetAdapter(PlatformConfig(enabled=True, extra={"host": "127.0.0.1", "port": 0, **extra}))


@requires_hermes
@pytest.mark.usefixtures("registered_platform")
def test_a_gadget_recording_is_transcribed_in_the_configured_language(monkeypatch):
    path = _record(_adapter(stt_language="pt"), monkeypatch)
    assert Path(path).exists()
    # Without stt.local.language, Hermes would auto-detect; with an English one it would force English.
    assert _transcribe(monkeypatch, path, {"local": {}}) == "pt"
    assert _transcribe(monkeypatch, path, {"local": {"language": "en"}}) == "pt"


@requires_hermes
@pytest.mark.usefixtures("registered_platform")
def test_auto_keeps_the_profiles_stt_settings(monkeypatch):
    path = _record(_adapter(), monkeypatch)
    assert _transcribe(monkeypatch, path, {"local": {}}) is None  # the backend reads stt.local itself


@requires_hermes
@pytest.mark.usefixtures("registered_platform")
def test_other_audio_keeps_the_profiles_stt_settings(monkeypatch, tmp_path):
    _record(_adapter(stt_language="pt"), monkeypatch)
    voice_note = tmp_path / "audio_0f0f0f0f0f0f.ogg"  # another platform's voice note
    voice_note.write_bytes(b"OggS")
    assert _transcribe(monkeypatch, str(voice_note), {"local": {}}) is None


def test_plugin_yaml_declares_every_hook_the_plugin_registers():
    # Hermes's catalog validation fails a plugin whose registered hooks aren't in provides_hooks.
    import re

    from conftest import REPO
    from hermes_gadget_plugin.dashboard import SessionBoard

    manifest = (REPO / "plugin" / "plugin.yaml").read_text(encoding="utf-8")
    block = re.search(r"(?m)^provides_hooks:.*\n((?:  - .*\n)+)", manifest).group(1)
    declared = {re.match(r"  - (\w+)", line).group(1) for line in block.splitlines()}
    ctx = _HookCtx()
    stt.register(ctx)
    assert declared == set(SessionBoard.HOOKS) | set(ctx.hooks)
