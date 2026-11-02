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

import logging
import re
from typing import Any, Optional

from . import runtime

logger = logging.getLogger(__name__)

# A language tag: a 2- or 3-letter code, optionally with subtags (pt, en, pt-BR, zh-Hant).
_TAG = re.compile(r"[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*")


def normalize(value: Any) -> Optional[str]:
    """The configured language, or None for ``auto`` (the profile's own STT settings apply).

    The primary code is lowercased (``PT`` -> ``pt``). Anything that isn't a language tag, such as
    ``Portuguese``, is refused with a warning: passed on, it would make every gadget transcription
    fail, where None keeps the profile's own STT settings."""
    language = str(value or "").strip()
    if language.lower() in ("", "auto"):
        return None
    if not _TAG.fullmatch(language):
        logger.warning("platforms.gadget.extra.stt_language %r is not a language code such as pt or pt-BR; "
                       "using each profile's own STT settings", language)
        return None
    primary, _, rest = language.partition("-")
    return primary.lower() + ("-" + rest if rest else "")


def pre_transcription(file_path: Optional[str] = None, **_: Any) -> Optional[dict]:
    """Hermes's ``pre_transcription`` hook: ``{"language": ...}`` for a gadget recording, else None."""
    language = runtime.clip_language(file_path)
    return {"language": language} if language else None


def register(ctx) -> None:
    """Register the hook in this profile's copy of the plugin."""
    ctx.register_hook("pre_transcription", pre_transcription)
