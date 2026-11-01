"""Fix the speech-to-text language of gadget recordings (``platforms.gadget.extra.stt_language``).

On a multiplexed gateway, a gadget's voice is transcribed with the ``stt`` settings of the profile
that answers it, and a profile without ``stt.<provider>.language`` leaves the language to
auto-detection, which can mistake short clips for English. The adapter notes each recording it
saves (``runtime.note_clip``); this ``pre_transcription`` hook, a public plugin hook, sets the
language for those recordings only. Other platforms' audio is left alone.

Hermes loads a copy of the plugin per profile and fires only the answering profile's hooks, so
every copy registers the hook and reads the process-wide table in ``runtime``.
"""

from __future__ import annotations

from typing import Any, Optional

from . import runtime


def normalize(value: Any) -> Optional[str]:
    """The configured language, or None for ``auto`` (the profile's own STT settings apply)."""
    language = str(value or "").strip()
    return None if language.lower() in ("", "auto") else language


def pre_transcription(file_path: Optional[str] = None, **_: Any) -> Optional[dict]:
    language = runtime.clip_language(file_path)
    return {"language": language} if language else None


def register(ctx) -> None:
    ctx.register_hook("pre_transcription", pre_transcription)
